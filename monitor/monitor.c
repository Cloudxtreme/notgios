/*----- System Includes -----*/

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <syslog.h>
#include <time.h>
#include <sys/wait.h>

/*----- Local Includes -----*/

#include "monitor.h"
#include "worker.h"
#include "../include/hash.h"
#include "../include/list.h"

/*----- Macro Declarations -----*/

// Macro zeros the given buffer, writes in the given message using sprintf, then returns
// from the function it was called in. Uses NOTGIOS_STATIC_BUFSIZE directly as the size of
// the buffer (instead of taking it as a parameter). Only meant to be called from the
// handle_add and handle_reschedule functions.
#define RETURN_NACK(buf, msg)                                               \
  do {                                                                      \
    write_log(LOG_ERR, "Monitor: Sending a NACK because of %s...\n", msg);  \
    memset(buf, 0, sizeof(char) * NOTGIOS_STATIC_BUFSIZE);                  \
    sprintf(buf, "NGS NACK\nCAUSE %s\n\n", msg);                            \
    return;                                                                 \
  } while (0);

#define RETURN_ACK(buf)                                                     \
  do {                                                                      \
    write_log(LOG_DEBUG, "Monitor: Sending an ACK...\n");                   \
    memset(buf, 0, sizeof(char) * NOTGIOS_STATIC_BUFSIZE);                  \
    sprintf(buf, "NGS ACK\n\n");                                            \
    return;                                                                 \
  } while (0);

// This is not supposed to be a generic max function, and has tons of pitfalls.
// Really just meant to return the greatest of two, not being incremented, ints.
#define SIMPLE_MAX(x, y) (((x) > (y)) ? (x) : (y))

/*----- Local Function Declarations -----*/

// Thread Management Functions
void *launch_worker_thread(void *args);
void handle_add(char **commands, char *reply_buf);
void handle_reschedule(char *cmd, char *reply_buf, task_action_t action);

// Signal Handlers
void handle_signal(int signo);
void handle_term();
void handle_child();

// High Level Network Functions
void send_reports(int socket);
int handle_process_report(task_report_t *report, char *start, char *buffer);
int handle_directory_report(task_report_t *report, char *start, char *buffer);
int handle_disk_report(task_report_t *report, char *start, char *buffer);
int handle_swap_report(task_report_t *report, char *start, char *buffer);
int handle_load_report(task_report_t *report, char *start, char *buffer);
int handle_total_report(task_report_t *report, char *start, char *buffer);

// Low Level Network Functions
int create_server(short port);
int handshake(char *server_hostname, int port, int initial, short monitor_port);
int handle_read(int fd, char *buffer, int len);
int handle_write(int fd, char *buffer);

// Utility Functions
int parse_commands(char **output, char *input);
thread_control_t *create_thread_control();
void destroy_thread_control(void *voidarg);
void remove_dead();
void increment_stats(task_type_t type, char *id);
void decrement_stats(task_type_t type, char *id);
void user_error();

/*----- Evil but Necessary Globals -----*/

hash_t threads, controls, children;
list_t reports;
monitor_stats_t task_stats;
pthread_rwlock_t stats_lock;
int termpipe_in, termpipe_out, exiting = 0;

/*----- Function Implementations -----*/

int main(int argc, char **argv) {
  int c, port = 0, server_socket = 0, initial = 1;
  char *server_hostname = NULL;

  // Parse command line args.
  opterr = 0;
  while ((c = getopt(argc, argv, "s:p:")) != -1) {
    switch (c) {
      case 's':
        server_hostname = optarg;
        break;
      case 'p':
        port = atoi(optarg);
        break;
      default:
        user_error();
        return EXIT_FAILURE;
    }
  }
  if (!server_hostname || !port) {
    user_error();
    return EXIT_FAILURE;
  }

  // Initializations.
#ifndef DEBUG
  openlog("Notgios Monitor", 0, 0);
#endif
  int retvals[4];
  retvals[0] = init_hash(&threads, free);
  retvals[1] = init_hash(&controls, destroy_thread_control);
  retvals[2] = init_hash(&children, free);
  retvals[3] = init_list(&reports, sizeof(task_report_t), free);
  if (retvals[0] || retvals[1] || retvals[2] || retvals[3]) {
    write_log(LOG_ERR, "Monitor: Failed to initialize necessary tables and lists, exiting...\n");
    return EXIT_FAILURE;
  }
  int pipes[2];
  if (pipe(pipes)) {
    write_log(LOG_ERR, "Monitor: Failed to open termination pipe, exiting...\n");
    return EXIT_FAILURE;
  }
  termpipe_out = pipes[0];
  termpipe_in = pipes[1];
  pthread_rwlock_init(&stats_lock, NULL);
  memset(&task_stats, 0, sizeof(monitor_stats_t));

  // Setup signal handlers.
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);

  // Ignore SIGINTs.
  retvals[0] = sigaction(SIGINT, &sa, NULL);
  retvals[1] = sigaction(SIGPIPE, &sa, NULL);

  // Handle SIGCHLD and SIGTERMs. Also, block those signals while handling them.
  sigaddset(&sa.sa_mask, SIGTERM);
  sa.sa_handler = handle_signal;
  retvals[2] = sigaction(SIGCHLD, &sa, NULL);
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGCHLD);
  retvals[3] = sigaction(SIGTERM, &sa, NULL);

  // Check out return values.
  if (retvals[0] || retvals[1] || retvals[2] || retvals[3]) {
    write_log(LOG_ERR, "Monitor: Failed to install all signal handlers, exiting...\n");
    return EXIT_FAILURE;
  }

  // Outer infinite loop to allow for exceptional conditions, like the server going down.
  while (1) {
    // Server will connect to us after we send over the port, so find one that works.
    short monitor_port = NOTGIOS_MONITOR_PORT - 1;
    int bound = 0;
    while (!bound && monitor_port - NOTGIOS_MONITOR_PORT < 20) {
      server_socket = create_server(++monitor_port);
      if (server_socket != NOTGIOS_SOCKET_FAILURE) {
        bound = 1;
        break;
      }
    }
    if (bound) {
      write_log(LOG_DEBUG, "Monitor: Successfully opened a listening socket on port %hd...\n", monitor_port);
    } else {
      write_log(LOG_ERR, "Monitor: Failed to open a listening socket, exiting...\n");
      user_error();
      return EXIT_FAILURE;
    }

    // Perform the handshake with the server and act accordingly.
    switch (handshake(server_hostname, port, initial, monitor_port)) {
      case NOTGIOS_SUCCESS:
        break;
      case NOTGIOS_SERVER_REJECTED:
        write_log(LOG_ERR, "Monitor: Server sent back a rejection message...\n");
        user_error();
      case NOTGIOS_BAD_HOSTNAME:
        write_log(LOG_ERR, "Monitor: Server doesn't appear to exist? Bad hostname...\n");
        user_error();
      default:
        write_log(LOG_ERR, "Monitor: Initial handshake with server failed, exiting...\n");
        return EXIT_FAILURE;
    }
    initial = 0;

    write_log(LOG_INFO, "Monitor: Initial handshake completed, waiting for server to connect...\n");
    struct timeval time;
    time.tv_sec = NOTGIOS_ACCEPT_TIMEOUT;
    time.tv_usec = 0;
    fd_set to_read;
    FD_ZERO(&to_read);
    FD_SET(server_socket, &to_read);
    select(server_socket + 1, &to_read, NULL, NULL, &time);

    // Select returned, so check if we've timed out.
    if (!FD_ISSET(server_socket, &to_read)) {
      write_log(LOG_ERR, "Monitor: Timed out while waiting for server to make contact, exiting...\n");
      return EXIT_FAILURE;
    }

    // We have a connection waiting, so grab it.
    // Also, handle potential race condition (should never come up, but we know how computers are) of
    // socket being closed in between select and accept.
    char buffer[NOTGIOS_STATIC_BUFSIZE];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);
    int socket = accept(server_socket, (struct sockaddr *) &client_addr, &client_len);
    if (socket < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        write_log(LOG_ERR, "Monitor: Server closed connection while it was being opened. Exiting to start clean...\n");
      } else {
        write_log(LOG_ERR, "Monitor: An unknown error occured while attempting to accept connection from server, exiting...\n");
      }
      return EXIT_FAILURE;
    }
    fcntl(socket, F_SETFL, O_NONBLOCK);
    write_log(LOG_INFO, "Monitor: Connected to server...\n");

    // We're connected. Start reading jobs and stuff from the server.
    while (!exiting) {
      int retval = handle_read(socket, buffer, NOTGIOS_STATIC_BUFSIZE);
      if (retval >= 0) {
        char *commands[NOTGIOS_REQUIRED_COMMANDS + NOTGIOS_MAX_OPTIONS];

        if (!parse_commands(commands, buffer)) {
          char *cmd = commands[0];
          if (strstr(cmd, "NGS JOB ADD") == cmd) {
            write_log(LOG_INFO, "Monitor: Received an add message...\n");
            handle_add(commands, buffer);
          } else if (strstr(cmd, "NGS JOB PAUS") == cmd) {
            write_log(LOG_INFO, "Monitor: Received a pause message...\n");
            handle_reschedule(commands[1], buffer, PAUSE);
          } else if (strstr(cmd, "NGS JOB RES") == cmd) {
            write_log(LOG_INFO, "Monitor: Received a resume message...\n");
            handle_reschedule(commands[1], buffer, RESUME);
          } else if (strstr(cmd, "NGS JOB DEL") == cmd) {
            write_log(LOG_INFO, "Monitor: Received a delete message...\n");
            handle_reschedule(commands[1], buffer, DELETE);
          } else if (strstr(cmd, "NGS STILL THERE?") == cmd) {
            // Manual keepalive. I know TCP is supposed to do stuff like this on its own, but honestly it makes
            // it easier on my end to detect errors if I also do it manually.
            write_log(LOG_INFO, "Monitor: Received keepalive message...\n");
            sprintf(buffer, "NGS STILL HERE!\n\n");
          } else if (strstr(cmd, "NGS BYE") == cmd) {
            write_log(LOG_INFO, "Monitor: Server send a shutdown message, beginning reconnect procedures...\n");
            break;
          } else if (exiting) {
            sprintf(buffer, "NGS NACK\nCAUSE SHUTDOWN\n\n");
          } else {
            // Shouldn't happen, but hey, everything that isn't supposed to happen eventually does, so there.
            write_log(LOG_ERR, "Monitor: Received an invalid message, discarding...\n");
            sprintf(buffer, "NGS NACK\nCAUSE UNRECOGNIZED_COMMAND\n\n");
          }
        } else {
          // The command we were sent is over the max size limit.
          // Could dynamically allocate memory, but, since we're taking input from the user for this, there should
          // be some sane upper limit to the command size anyways, in case they manage to slip something clever in,
          // so this will indicate that possibility.
          sprintf(buffer, "NGS NACK\nCAUSE COMMAND_TOO_LONG\n\n");
        }

        // Write our reply.
        handle_write(socket, buffer);

        // Check the report queue for entries and send them if they exist.
        // This should probably be handled by a dedicated thread, but having two threads writing into the same socket
        // is almost impossible to get right, and having two sockets requires significantly more error handling.
        // Either way, due to the keepalive, except in exceptional circumstances when we wouldn't be able to write
        // data anyways, this is gauranteed to be run at least once every 10 seconds.
        send_reports(socket);

        // If any tasks encountered unrecoverable errors, a message has already been sent to the front end, so remove
        // the task from the tables.
        remove_dead();
      } else {
        write_log(LOG_ERR, "Monitor: Error reading from socket...\n");
        break;
      }
    }

    // Either our socket has been unexpectedly closed (server crashed), we received an orderly
    // shutdown message from the server (server was interrupted by user, or machine server was
    // running on was shutdown), or we received a SIGTERM.
    // If the socket closed or the serve shutdown, we need to start buffering output and attempting to
    // reopen the connection with the server.
    // If we received a SIGTERM, we need to stop all tasks and shutdown.
    if (exiting) {
      write_log(LOG_INFO, "Monitor: Shutdown was initiated. Killing tasks...\n");

      // FIXME: Need to handle the possibility of user sending us a SIGTERM for the hell of it, leaving the child running,
      // causing the keepalive logic to fail when we come back up.
      char **tasks = hash_keys(&threads);
      for (char **current = tasks, *task_id = *current; current - tasks < threads.count; current++, task_id = *current) {
        pthread_t *thread = hash_get(&threads, task_id);
        thread_control_t *control = hash_get(&controls, task_id);

        // Synchronize and set exit flag.
        pthread_mutex_lock(&control->mutex);
        control->paused = 0;
        control->killed = 1;
        pthread_cond_signal(&control->signal);
        pthread_mutex_unlock(&control->mutex);

        // Join with task thread.
        pthread_join(*thread, NULL);
        write_log(LOG_INFO, "Monitor: Killed a task...\n");
      }
      write_log(LOG_INFO, "Monitor: Tasks have exited, proceeding to shutdown...\n");
      free(tasks);
      destroy_hash(&threads);
      destroy_hash(&controls);
      destroy_hash(&children);
      handle_write(socket, "NGS BYE\n\n");
    }

    close(socket);
    close(server_socket);
    if (exiting) return EXIT_SUCCESS;
  }
}

void *launch_worker_thread(void *voidargs) {
  // Parse out all of the relevant arguments.
  thread_args_t *args = voidargs;
  struct timespec time;
  char *id = args->id;
  int freq = args->freq;
  task_type_t type = args->type;
  metric_type_t metric = args->metric;
  thread_control_t *control = args->control;
  task_option_t *options = args->options;

  // Update stats to reflect task creation.
  increment_stats(type, id);

  // Spin and collect data until we're killed.
  write_log(LOG_INFO, "Task %s: Successfully launched!\n", id);
  pthread_mutex_lock(&control->mutex);
  while (!control->killed) {
    // Check if we've been paused, and sleep until we're rescheduled if so.
    if (control->paused) pthread_cond_wait(&control->signal, &control->mutex);

    // Make the magic happen.
    int retval = run_task(type, metric, options, id);

    // Check error conditions.
    if (retval == NOTGIOS_TASK_FATAL) {
      // If we encountered a fatal error, message to front end has already been queued, so
      // remove the task.
      write_log(LOG_ERR, "Task %s: Encountered a fatal error, exiting...\n", id);
      control->dropped = 1;
      pthread_mutex_unlock(&control->mutex);

      // Update stats to reflect task removal.
      decrement_stats(type, id);
      return NULL;
    } else if (retval == NOTGIOS_GENERIC_ERROR) {
      // This shouldn't happen, but would indicate that an incorrectly initialized task slipped
      // into things. Keeping around for debugging purposes.
      write_log(LOG_ERR, "Task %s: Initialization of task appears invalid, exiting...\n", id);
      control->dropped = 1;
      pthread_mutex_unlock(&control->mutex);

      // Update stats to reflect task removal.
      decrement_stats(type, id);
      return NULL;
    }
    write_log(LOG_INFO, "Task %s: Finished collecting data...\n", id);

    // pthread_cond_timewait takes an absolute time to sleep until. Seems kind of silly to me,
    // but we have to deal with it.
    // FIXME: Apparently this function was added to POSIX recently enough to not be portable.
    // Need to replace with clock function. Fails to compile on CentOS 6.6.
    clock_gettime(CLOCK_REALTIME, &time);
    time.tv_sec += freq;

    // Sleep until either it's time to collect data again or we've been rescheduled.
    pthread_cond_timedwait(&control->signal, &control->mutex, &time);
  }
  pthread_mutex_unlock(&control->mutex);

  // Update stats to reflect task removal.
  decrement_stats(type, id);

  return NULL;
}

// Function takes care of adding a task.
void handle_add(char **commands, char *reply_buf) {
  int freq;
  char type_str[NOTGIOS_MAX_TYPE_LEN], metric_str[NOTGIOS_MAX_METRIC_LEN], id[NOTGIOS_MAX_NUM_LEN];
  task_type_t type;
  metric_type_t metric;

  // Pull out the parameters.
  sscanf(commands[1], "ID %s", id);
  sscanf(commands[2], "TYPE %s", type_str);
  sscanf(commands[3], "METRIC %s", metric_str);
  sscanf(commands[4], "FREQ %d", &freq);

  // "Convert" from string to enum value.
  if (!strcmp(type_str, "PROCESS")) type = PROCESS;
  else if (!strcmp(type_str, "DIRECTORY")) type = DIRECTORY;
  else if (!strcmp(type_str, "DISK")) type = DISK;
  else if (!strcmp(type_str, "SWAP")) type = SWAP;
  else if (!strcmp(type_str, "LOAD")) type = LOAD;
  else RETURN_NACK(reply_buf, "UNRECOGNIZED_TYPE");

  // "Convert" from string to enum value.
  if (!strcmp(metric_str, "MEMORY")) metric = MEMORY;
  else if (!strcmp(metric_str, "CPU")) metric = CPU;
  else if (!strcmp(metric_str, "IO")) metric = IO;
  else if (!strcmp(metric_str, "NONE")) metric = NONE;
  else RETURN_NACK(reply_buf, "UNRECOGNIZED_METRIC");

  // This shouldn't happen, but would mean that the server sent us a duplicate ID.
  if (hash_get(&threads, id) != NULL) RETURN_NACK(reply_buf, "DUPLICATE_ID");

  // Get information ready to pass onto the thread.
  thread_args_t *arguments = calloc(1, sizeof(thread_args_t));
  strcpy(arguments->id, id);
  arguments->freq = freq;
  arguments->type = type;
  arguments->metric = metric;

  // Look over rest of commands if applicable.
  // God this is ugly, but it's the best I can come up with.
  if (type == PROCESS || type == DIRECTORY || type == DISK) {
    int elem = 0;

    // Declare arrays so that the indexes of each correspond with each other.
    char *option_strings[] = {
      "KEEPALIVE",
      "PIDFILE",
      "RUNCMD",
      "PATH",
      "MNTPNT"
    };
    task_option_type_t options[] = {
      KEEPALIVE,
      PIDFILE,
      RUNCMD,
      PATH,
      MNTPNT
    };
    task_type_t option_categories[] = {
      PROCESS,
      PROCESS,
      PROCESS,
      DIRECTORY,
      DISK
    };
    int num_options = sizeof(option_strings) / sizeof(option_strings[0]);

    for (int i = 5; commands[i] && i < 5 + NOTGIOS_MAX_OPTIONS; i++) {
      int found = 0;
      char *cmd = commands[i];

      for (int j = 0; j < num_options; j++) {
        char *option_type = option_strings[j];

        // Figure out which option we're using.
        if (strstr(cmd, option_type) == cmd) {
          // Check that the option applies to this type of task.
          if (type != option_categories[j]) {
            free(arguments);
            RETURN_NACK(reply_buf, "INAPPLICABLE_OPTION");
          }
          found = 1;

          // Everything is kosher. Assign the enum and copy over the parameter from the command.
          task_option_t option;
          memset(&option, 0, sizeof(task_option_t));
          int option_len = strlen(option_type), cmd_len = strlen(cmd);
          char *copy_start = cmd + option_len + 1;
          int copy_len = cmd_len - option_len;

          option.type = options[j];
          memcpy(&option.value, copy_start, copy_len);
          arguments->options[elem++] = option;
        }
      }

      if (!found) {
        // Our option was never found! We've been given an invalid option.
        // Should never happen.
        free(arguments);
        RETURN_NACK(reply_buf, "UNRECOGNIZED_OPTION");
      }
    }
  }
  write_log(LOG_DEBUG, "Monitor: Finished parsing options. Starting the task...\n");

  // Create a control struct for the thread.
  thread_control_t *control = create_thread_control();
  arguments->control = control;

  // Create a new thread to run the task!
  pthread_t *task = malloc(sizeof(pthread_t));
  pthread_create(task, NULL, launch_worker_thread, arguments);

  // Add our new thread and its controls.
  int retvals[2];
  retvals[0] = hash_put(&threads, id, task);
  retvals[1] = hash_put(&controls, id, control);

  if (retvals[0] == HASH_FROZEN || retvals[1] == HASH_FROZEN) {
    // This can only happen if we've received a SIGTERM.
    RETURN_NACK(reply_buf, "SHUTDOWN");
  }
  // Write acknowledgement.
  RETURN_ACK(reply_buf);
}

// Function handles pausing, resuming, and deleting tasks.
void handle_reschedule(char *cmd, char *reply_buf, task_action_t action) {
  char id_str[NOTGIOS_MAX_NUM_LEN];
  thread_control_t *control;
  pthread_t *task;

  // Get task to reschedule.
  memset(id_str, 0, sizeof(char) * NOTGIOS_MAX_NUM_LEN);
  sscanf(cmd, "ID %s", id_str);
  control = hash_get(&controls, id_str);

  // This shouldn't happen, but would mean the server sent us a request for a nonexistent ID.
  if (!control) RETURN_NACK(reply_buf, "NO_SUCH_ID");

  // Synchronize threads and set status.
  pthread_mutex_lock(&control->mutex);
  switch (action) {
    case PAUSE:
      control->paused = 1;
      break;
    case RESUME:
      control->paused = 0;
      break;
    case DELETE:
      control->killed = 1;
      control->paused = 0;
  }
  pthread_cond_signal(&control->signal);
  pthread_mutex_unlock(&control->mutex);

  if (action == DELETE) {
    task = hash_get(&threads, id_str);
    pthread_join(*task, NULL);
    int retval = hash_drop(&threads, id_str);

    // This can only happen if we've received a SIGTERM.
    if (retval == HASH_FROZEN) RETURN_NACK(reply_buf, "SHUTDOWN");
  }

  // Write acknowledgement.
  write_log(LOG_INFO, "Monitor: Task successfully rescheduled...\n");
  RETURN_ACK(reply_buf);
}

void handle_signal(int signo) {
  if (signo == SIGCHLD) handle_child();
  else if (signo == SIGTERM) handle_term();
}

void handle_term() {
  // We're shutting down, so freeze the hashes so they can't be modified.
  hash_freeze(&threads);
  hash_freeze(&controls);
  hash_freeze(&children);

  // Set the exiting flag.
  exiting = 1;

  // FIXME: HACK ALERT!!!!
  // I can't shut threads down or do anything complicated here, because I'm in a signal handler,
  // so I need the main thread to notice and do it for me. Problem is, the main thread could be
  // blocked on input from the server for up to 20 seconds. I like the current read behavior,
  // so I can't really change that, so, instead I've set up a pipe that can be written to during
  // shutdown for the sole purpose of forcing the select call in handle_read to return.
  // It doesn't matter what's written, as long as something is, so this just spins until write
  // reports that it's written at least one byte.
  //
  // If you have a better idea for how I could do this, shoot me an email at cfretz@icloud.com
  int actual = 0;
  while (!actual) actual += write(termpipe_in, "halt!", 6);
}

// Function handles removing dead children from the children hash so that they'll be restarted.
// TODO: Need to check exit status of child here to see if the exec failed.
void handle_child() {
  int status = 0;
  pid_t pid = waitpid((pid_t) -1, &status, WUNTRACED | WCONTINUED);

  // Iterate across all of the current child processes to find the one we're being signaled about.
  char **tasks = hash_keys(&children), *id = NULL;
  for (char **current = tasks, *task_id = *current; current - tasks < children.count; current++, task_id = *current) {
    uint16_t *tmp_pid = hash_get(&children, task_id);
    if (*tmp_pid == pid) {
      id = task_id;
      break;
    }
  }

  // This shouldn't happen, and I don't know what to do if it did, but I'll put this here for debugging purposes.
  if (!id) {
    free(tasks);
    write_log(LOG_ERR, "Monitor: Was sent a SIGCHLD, but can't find it???\n");
    return;
  }

  // Figure out what happened to the child and take appropriate action.
  if (WIFEXITED(status) || WIFSIGNALED(status)) {
    write_log(LOG_INFO, "Child for task %s either crashed, exited, or was killed. Marking for restart...\n", id);
    hash_drop(&children, id);
  } else if (WIFSTOPPED(status)) {
    // TODO: User might not want this, so need to make it configurable.
    write_log(LOG_INFO, "Child for task %s was stopped, sending a SIGCONT...\n", id);
    kill(pid, SIGCONT);
  }
  free(tasks);
}

void send_reports(int socket) {
  while (reports.count > 0) {
    task_report_t report;
    int retval = NOTGIOS_SUCCESS;
    char buffer[NOTGIOS_STATIC_BUFSIZE], *start = "NGS JOB REPORT";
    rpop(&reports, &report);

    if (strlen(report.message) == 0) {
      // Task is good.
      switch (report.type) {
        case PROCESS:
          retval = handle_process_report(&report, start, buffer);
          break;
        case DIRECTORY:
          retval = handle_directory_report(&report, start, buffer);
          break;
        case DISK:
          retval = handle_disk_report(&report, start, buffer);
          break;
        case SWAP:
          retval = handle_swap_report(&report, start, buffer);
          break;
        case LOAD:
          retval = handle_load_report(&report, start, buffer);
          break;
        case TOTAL:
          retval = handle_total_report(&report, start, buffer);
          break;
        default:
          write_log(LOG_DEBUG, "Monitor: Found an invalid report while sending reports...\n");
          retval = NOTGIOS_GENERIC_ERROR;
      }
    } else {
      // Task encountered an error.
      sprintf(buffer, "%s\nID %s\n%s\n\n", start, report.id, report.message);
    }

    if (retval == NOTGIOS_SUCCESS) {
      // Send the report to the server.
      // FIXME: Currently does not expect an ACK after sending reports. Simplifies logic as this
      // could otherwise potentially swallow keepalive messages or other things.
      handle_write(socket, buffer);
    }
  }
}

int handle_process_report(task_report_t *report, char *start, char *buffer) {
  char specific_msg[NOTGIOS_SMALL_BUFSIZE];

  // Write our metric specific message.
  switch (report->metric) {
    case MEMORY:
      sprintf(specific_msg, "BYTES %d", (int) report->value);
      break;
    case CPU:
      sprintf(specific_msg, "CPU PERCENT %.2f", report->percentage);
      break;
    case IO:
      sprintf(specific_msg, "IO PERCENT %.2f", report->percentage);
      break;
    default:
      write_log(LOG_DEBUG, "Monitor: Found an invalid process report while sending reports...\n");
      return NOTGIOS_GENERIC_ERROR;
  }

  // Write the full message.
  sprintf(buffer, "%s\nID %s\n%s\n\n", start, report->id, specific_msg);
  return NOTGIOS_SUCCESS;
}

int handle_directory_report(task_report_t *report, char *start, char *buffer) {
  if (report->metric != MEMORY) {
    write_log(LOG_DEBUG, "Monitor: Found an invalid directory report while sending reports...\n");
    return NOTGIOS_GENERIC_ERROR;
  }
  sprintf(buffer, "%s\nID %s\nBYTES %ld\n\n", start, report->id, (long) report->value);
  return NOTGIOS_SUCCESS;
}

int handle_disk_report(task_report_t *report, char *start, char *buffer) {
  // TODO: Implement this function.
}

int handle_swap_report(task_report_t *report, char *start, char *buffer) {
  // TODO: Implement this function.
}

int handle_load_report(task_report_t *report, char *start, char *buffer) {
  // TODO: Implement this function.
}

int handle_total_report(task_report_t *report, char *start, char *buffer) {
  // TODO: Implement this function.
}

// Performs handshake with server. Two different types of handshakes are possible and denote
// slightly different things.
// The first type happens during initial startup, and the other if the server goes down for
// any reason. The first type assumes the server will send over all tasks for this host, and
// the second does not.
int handshake(char *server_hostname, int port, int initial, short monitor_port) {
  int sockfd, sleep_period = 1;
  struct sockaddr_in serv_addr;
  struct hostent *server;
  char buffer[NOTGIOS_STATIC_BUFSIZE];

  // Begin long and arduous connection process...
  sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) return NOTGIOS_GENERIC_ERROR;
  server = gethostbyname(server_hostname);
  if (!server) return NOTGIOS_BAD_HOSTNAME;
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
  serv_addr.sin_port = htons(port);
  write_log(LOG_DEBUG, "Monitor: Attempting to connect to server...\n");
  while (connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
    write_log(LOG_ERR, "Monitor: Connect failed, sleeping for %d seconds...\n", sleep_period);
    sleep(sleep_period);
    if (sleep_period < 32) sleep_period *= 2;
    if (sleep_period == 32 && initial) return NOTGIOS_SERVER_UNREACHABLE;
  }

  // Send hello message to server.
  memset(buffer, 0, sizeof(char) * NOTGIOS_STATIC_BUFSIZE);
  if (initial) sprintf(buffer, "NGS HELLO\nCMD PORT %hd\n\n", monitor_port);
  else sprintf(buffer, "NGS HELLO AGAIN\nCMD PORT %hd\n\n", monitor_port);
  handle_write(sockfd, buffer);

  // Get server's response.
  handle_read(sockfd, buffer, NOTGIOS_STATIC_BUFSIZE);
  if (strstr(buffer, "NGS ACK") == buffer) {
    close(sockfd);
    return NOTGIOS_SUCCESS;
  } else if (strstr(buffer, "NGS NACK") == buffer) {
    close(sockfd);
    return NOTGIOS_SERVER_REJECTED;
  } else {
    close(sockfd);
    return NOTGIOS_GENERIC_ERROR;
  }
}


// Should be an extremely fault tolerant wrapper around read.
// Keeps reading until encountering a double newline.
// Zeros buffer before reading to it.
// Can block for a maximum of NOTGIOS_READ_TIMEOUT. If no data is available at the
// end of the timeout, assumes there is a problem with the socket, and returns
// NOTGIOS_SOCKET_CLOSED.
int handle_read(int fd, char *buffer, int len) {
  int actual = 0, e_count = 0;
  memset(buffer, 0, sizeof(char) * len);
  while (actual < 2 || buffer[strlen(buffer) - 1] != '\n' || buffer[strlen(buffer) - 2] != '\n') {
    fd_set to_read;
    FD_ZERO(&to_read);
    FD_SET(fd, &to_read);
    FD_SET(termpipe_out, &to_read);
    struct timeval time;
    time.tv_sec = NOTGIOS_READ_TIMEOUT;
    time.tv_usec = 0;

    select(SIMPLE_MAX(fd, termpipe_out) + 1, &to_read, NULL, NULL, &time);
    if (FD_ISSET(fd, &to_read)) {
      // We've received data from the server, time to read it.
      int retval = read(fd, buffer, len);
      if (retval > 0) {
        e_count = 0;
        actual += retval;
      } else if (retval == 0 || e_count++ > 5) {
        return NOTGIOS_SOCKET_CLOSED;
      }
    } else if (FD_ISSET(termpipe_out, &to_read)) {
      // Data was written from the SIGTERM handler. We're in shutdown. Erase anything that was
      // read off the socket (make sure we don't pass along partial data) and return immediately.
      memset(buffer, 0, sizeof(char) * len);
      return 0;
    } else {
      return NOTGIOS_SOCKET_CLOSED;
    }
  }
  return actual;
}

// Should be an extremely fault tolerant wrapper around write.
// Expects buffer to be an null terminated string, uses strlen to calculate the expected
// number of bytes to be written, and keeps calling write until that number has been written.
// If write fails due to blocking concerns, function assumes this means the write buffer is
// full, and that either we're writing data too quickly, or the remote end has experienced an
// unexpected shutdown. In an attempt to disambiguate this, function blocks for a maximum of
// NOTGIOS_WRITE_TIMEOUT, before either writing the data or returning NOTGIOS_SOCKET_CLOSED.
int handle_write(int fd, char *buffer) {
  int actual = 0, expected = strlen(buffer);
  while (actual != expected) {
    int retval = write(fd, buffer, expected - actual);
    if (retval >= 0) {
      actual += retval;
    } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
      fd_set to_write;
      FD_ZERO(&to_write);
      FD_SET(fd, &to_write);
      struct timeval time;
      time.tv_sec = NOTGIOS_WRITE_TIMEOUT;
      time.tv_usec = 0;
      select(fd + 1, NULL, &to_write, NULL, &time);
      if (!FD_ISSET(fd, &to_write)) return NOTGIOS_SOCKET_CLOSED;
    } else if (errno == EPIPE) {
      return NOTGIOS_SOCKET_CLOSED;
    }
  }
  return actual;
}

// Function opens a listening socket on NOTGIOS_MONITOR_PORT and marks it as
// nonblocking.
int create_server(short port) {
  int server_fd;
  struct sockaddr_in serv_addr;
  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  fcntl(server_fd, F_SETFL, O_NONBLOCK);
  if (server_fd < 0) return NOTGIOS_SOCKET_FAILURE;
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(port);
  if (bind(server_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) return NOTGIOS_SOCKET_FAILURE;
  listen(server_fd, 10);
  return server_fd;
}

thread_control_t *create_thread_control() {
  thread_control_t *control = malloc(sizeof(thread_control_t));

  if (control) {
    control->paused = 0;
    control->killed = 0;
    control->dropped = 0;
    pthread_cond_init(&control->signal, NULL);
    pthread_mutex_init(&control->mutex, NULL);
  }

  return control;
}

void destroy_thread_control(void *voidarg) {
  thread_control_t *control = voidarg;
  if (control) {
    pthread_cond_broadcast(&control->signal);
    pthread_cond_destroy(&control->signal);
    pthread_mutex_destroy(&control->mutex);
    free(control);
  }
}

int parse_commands(char **output, char *input) {
  int elem = 0;
  memset(output, 0, sizeof(char *) * (NOTGIOS_REQUIRED_COMMANDS + NOTGIOS_MAX_OPTIONS));
  char *current = strtok(input, "\n");
  do {
    if (elem == 16) return NOTGIOS_TOO_MANY_ARGS;
    output[elem++] = current;
  } while ((current = strtok(NULL, "\n")));
  return NOTGIOS_SUCCESS;
}

void remove_dead() {
  char **tasks = hash_keys(&threads);
  for (char **current = tasks, *task_id = *current; current - tasks < threads.count; current++, task_id = *current) {
    pthread_t *thread = hash_get(&threads, task_id);
    thread_control_t *control = hash_get(&controls, task_id);

    // Not necessary to acquire lock. We only remove the task if dropped is set, in which case the thread is no longer
    // running, and if we happen to read dropped while it's being set, it'll be cleaned up next round.
    // Futhermore, we're the only thread that removes tasks.
    if (control->dropped) {
      write_log(LOG_INFO, "Monitor: Removing a dead task...\n");
      pthread_join(*thread, NULL);
      hash_drop(&threads, task_id);
      hash_drop(&controls, task_id);

      // Currently tasks can only really fail if there's like a serious problem with the system setup (unsupported distro)
      // or if there's an unrecoverable error that meant the task couldn't be recovered. Should never be any children
      // running, but whatever.
      hash_drop(&children, task_id);
    }
  }
  free(tasks);
  write_log(LOG_DEBUG, "Monitor: Finished cleaning up dead tasks...\n");
}

void increment_stats(task_type_t type, char *id) {
  pthread_rwlock_wrlock(&stats_lock);
  task_stats.num_tasks++;
  switch (type) {
    case PROCESS:
      task_stats.num_process_tasks++;
      break;
    case DIRECTORY:
      task_stats.num_dir_tasks++;
      break;
    case DISK:
      task_stats.num_disk_tasks++;
      break;
    case SWAP:
      task_stats.num_swap_tasks++;
      break;
    case LOAD:
      task_stats.num_load_tasks++;
      break;
    case TOTAL:
      task_stats.num_total_tasks++;
      break;
    default:
      write_log(LOG_DEBUG, "Task %s: Invalid task encountered during stat update...\n", id);
  }
  pthread_rwlock_unlock(&stats_lock);
}

void decrement_stats(task_type_t type, char *id) {
  pthread_rwlock_wrlock(&stats_lock);
  task_stats.num_tasks--;
  switch (type) {
    case PROCESS:
      task_stats.num_process_tasks--;
      break;
    case DIRECTORY:
      task_stats.num_dir_tasks--;
      break;
    case DISK:
      task_stats.num_disk_tasks--;
      break;
    case SWAP:
      task_stats.num_swap_tasks--;
      break;
    case LOAD:
      task_stats.num_load_tasks--;
      break;
    case TOTAL:
      task_stats.num_total_tasks--;
      break;
    default:
      write_log(LOG_DEBUG, "Task %s: Invalid task encountered during stat update...\n", id);
  }
  pthread_rwlock_unlock(&stats_lock);
}

void user_error() {
  fprintf(stderr, "This utility is used internally by the Notgios host monitoring framework, and is not meant to be launched manually.\n");
  exit(EINVAL);
}
