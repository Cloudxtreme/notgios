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

/*----- Local Includes -----*/

#include "monitor.h"
#include "worker.h"
#include "../include/hash.h"

/*----- Macro Declarations -----*/

// Macro zeros the given buffer, writes in the given message using sprintf, then returns
// from the function it was called in. Uses NOTGIOS_STATIC_BUFSIZE directly as the size of
// the buffer (instead of taking it as a parameter). Only meant to be called from the
// handle_add and handle_reschedule functions.
#define RETURN_NACK(buf, msg)                                       \
  do {                                                              \
    memset(buf, 0, sizeof(char) * NOTGIOS_STATIC_BUFSIZE);          \
    sprintf(buf, "NGS NACK\nCAUSE %s\n\n", msg);                    \
    return;                                                         \
  } while (0);

#define RETURN_ACK(buf)                                             \
  do {                                                              \
    memset(buf, 0, sizeof(char) * NOTGIOS_STATIC_BUFSIZE);          \
    sprintf(buf, "NGS ACK\n\n");                                    \
    return;                                                         \
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

// Network Functions
int handshake(char *server_hostname, int port, int initial);
int handle_read(int fd, char *buffer, int len);
int handle_write(int fd, char *buffer);

// Utility Functions
int create_server();
int parse_commands(char **output, char *input);
thread_control_t *create_thread_control();
void destroy_thread_control(void *voidarg);
void user_error();

/*----- Evil but Necessary Globals -----*/

hash_t threads, controls, children;
pthread_rwlock_t connection_lock;
int connected = 0, termpipe_in, termpipe_out, exiting = 0;

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
  openlog("Notgios Monitor", 0, 0);
  init_hash(&threads, free);
  init_hash(&controls, destroy_thread_control);
  init_hash(&children, free);
  pthread_rwlock_init(&connection_lock, NULL);
  int pipes[2];
  if (pipe(pipes)) {
    syslog(LOG_ERR, "Failed to open termination pipe, exiting...\n");
    return EXIT_FAILURE;
  }
  termpipe_out = pipes[0];
  termpipe_in = pipes[1];

  // Setup signal handlers.
  int retvals[3];
  struct sigaction sa;
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = 0;
  sigemptyset(&sa.sa_mask);

  // Ignore SIGINTs.
  retvals[0] = sigaction(SIGINT, &sa, NULL);

  // Handle SIGCHLD and SIGTERMs. Also, block those signals while handling them.
  sigaddset(&sa.sa_mask, SIGTERM);
  sa.sa_handler = handle_signal;
  retvals[1] = sigaction(SIGCHLD, &sa, NULL);
  sigemptyset(&sa.sa_mask);
  sigaddset(&sa.sa_mask, SIGCHLD);
  retvals[2] = sigaction(SIGTERM, &sa, NULL);

  // Check out return values.
  if (retvals[0] || retvals[1] || retvals[2]) {
    syslog(LOG_ERR, "Failed to install all signal handlers, exiting...\n");
    return EXIT_FAILURE;
  }

  // Outer infinite loop to allow for exceptional conditions, like the server going down.
  while (1) {
    // Perform the handshake with the server and act accordingly.
    switch (handshake(server_hostname, port, initial)) {
      case 0:
        break;
      case NOTGIOS_SERVER_REJECTED:
      case NOTGIOS_BAD_HOSTNAME:
        user_error();
      default:
        syslog(LOG_ERR, "Initial handshake with server failed, exiting...\n");
        return EXIT_FAILURE;
    }
    initial = 0;

    // The handshake was successful, configure the listening socket and wait on a connection.
    if ((server_socket = create_server()) == NOTGIOS_SOCKET_FAILURE) {
      user_error();
      syslog(LOG_ERR, "Failed to open listening socket, exiting...\n");
      return EXIT_FAILURE;
    }
    syslog(LOG_INFO, "Initial handshake completed, waiting for server to connect...\n");
    struct timeval time;
    time.tv_sec = NOTGIOS_ACCEPT_TIMEOUT;
    time.tv_usec = 0;
    fd_set to_read;
    FD_ZERO(&to_read);
    FD_SET(server_socket, &to_read);
    select(server_socket + 1, &to_read, NULL, NULL, &time);

    // Select returned, so check if we've timed out.
    if (!FD_ISSET(server_socket, &to_read)) {
      syslog(LOG_ERR, "Timed out while waiting for server to make contact, exiting...\n");
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
        syslog(LOG_ERR, "Server closed connection while it was being opened. Exiting to start clean...\n");
      } else {
        syslog(LOG_ERR, "An unknown error occured while attempting to accept connection from server, exiting...\n");
      }
      return EXIT_FAILURE;
    }
    fcntl(socket, F_SETFL, O_NONBLOCK);

    // Mark the global connected flag to make sure workers send their findings.
    pthread_rwlock_wrlock(&connection_lock);
    connected = 1;
    pthread_rwlock_unlock(&connection_lock);

    // We're connected. Start reading jobs and stuff from the server.
    while (!exiting) {
      int retval = handle_read(socket, buffer, NOTGIOS_STATIC_BUFSIZE);
      if (retval >= 0) {
        char *commands[NOTGIOS_REQUIRED_COMMANDS + NOTGIOS_MAX_OPTIONS];

        if (!parse_commands(commands, buffer)) {
          char *cmd = commands[0];
          if (strstr(cmd, "NGS JOB ADD") == cmd) {
            handle_add(commands, buffer);
          } else if (strstr(cmd, "NGS JOB PAUS") == cmd) {
            handle_reschedule(commands[1], buffer, PAUSE);
          } else if (strstr(cmd, "NGS JOB RES") == cmd) {
            handle_reschedule(commands[1], buffer, RESUME);
          } else if (strstr(cmd, "NGS JOB DEL") == cmd) {
            handle_reschedule(commands[1], buffer, DELETE);
          } else if (strstr(cmd, "NGS STILL THERE?") == cmd) {
            // Manual keepalive. I know TCP is supposed to do stuff like this on its own, but honestly it makes
            // it easier on my end to detect errors if I also do it manually.
            sprintf(buffer, "NGS STILL HERE!\n\n");
          } else if (strstr(cmd, "NGS BYE") == cmd) {
            break;
          } else if (exiting) {
            sprintf(buffer, "NGS NACK\nCAUSE SHUTDOWN\n\n");
          } else {
            // Shouldn't happen, but hey, everything that isn't supposed to happen eventually does, so there.
            sprintf(buffer, "NGS NACK\nCAUSE UNRECOGNIZED_COMMAND\n\n");
          }
        } else {
          // The command we were sent is over the max size limit.
          // Could dynamically allocate memory, but, since we're taking input from the user for this, there should
          // be some sane upper limit to the command size anyways, in case they manage to slip something clever in,
          // so this will indicate that possibility.
          sprintf(buffer, "NGS NACK\nCAUSE COMMAND_TOO_LONG\n\n");
        }

        handle_write(socket, buffer);
      } else {
        break;
      }
    }

    // Either our socket has been unexpectedly closed (server crashed), we received an orderly
    // shutdown message from the server (server was interrupted by user, or machine server was
    // running on was shutdown), or we received a SIGTERM.
    // If the socket closed or the serve shutdown, we need to start buffering output and attempting to
    // reopen the connection with the server.
    // If we received a SIGTERM, we need to stop all tasks and shutdown.
    if (!exiting) {
      // FIXME: I need to read up on socket behavior.
      pthread_rwlock_wrlock(&connection_lock);
      connected = 0;
      pthread_rwlock_unlock(&connection_lock);
    } else {
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
      }
      free(tasks);
      destroy_hash(&threads);
      destroy_hash(&controls);
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
  char *id = args->id;
  int freq = args->freq;
  task_type_t type = args->type;
  metric_type_t metric = args->metric;
  thread_control_t *control = args->control;
  task_option_t *options = args->options;

  // Setup our sleep timer.
  struct timespec time;
  time.tv_sec = freq;

  // Spin and collect data until we're killed.
  pthread_mutex_lock(&control->mutex);
  while (!control->killed) {
    // Check if we've been paused, and sleep until we're rescheduled if so.
    if (control->paused) pthread_cond_wait(&control->signal, &control->mutex);

    // Make the magic happen.
    run_task(type, metric, options, id);

    // Sleep until either it's time to collect data again or we've been rescheduled.
    pthread_cond_timedwait(&control->signal, &control->mutex, &time);
  }
  pthread_mutex_unlock(&control->mutex);

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
      MNTPNT,
      PATH
    };
    task_type_t option_categories[] = {
      PROCESS,
      PROCESS,
      PROCESS,
      DIRECTORY,
      DISK
    };
    int num_options = sizeof(option_strings) / sizeof(option_strings[0]);

    for (int i = 5; commands[i] && i < NOTGIOS_MAX_OPTIONS; i++) {
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
  } else if (retvals[0] == HASH_EXISTS || retvals[1] == HASH_EXISTS) {
    // This shouldn't happen, but would mean that the server sent us a duplicate ID.
    RETURN_NACK(reply_buf, "DUPLICATE_ID");
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

void handle_child() {
  // TODO: Not entirely clearcut what this function should do. Figure it out.
}

// Performs handshake with server. Two different types of handshakes are possible and denote
// slightly different things.
// The first type happens during initial startup, and the other if the server goes down for
// any reason. The first type assumes the server will send over all tasks for this host, and
// the second does not.
int handshake(char *server_hostname, int port, int initial) {
  int sockfd, actual = 0, sleep_period = 1;
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
  syslog(LOG_INFO, "Attempting to connect to server...\n");
  while (connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
    syslog(LOG_ERR, "Connect failed, sleeping for %d seconds...\n", sleep_period);
    sleep(sleep_period);
    if (sleep_period < 32) sleep_period *= 2;
    if (sleep_period == 32 && initial) return NOTGIOS_SERVER_UNREACHABLE;
  }

  // Send hello message to server.
  memset(buffer, 0, sizeof(char) * NOTGIOS_STATIC_BUFSIZE);
  if (initial) sprintf(buffer, "NGS HELLO\nCMD PORT %d\n\n", NOTGIOS_MONITOR_PORT);
  else sprintf(buffer, "NGS HELLO AGAIN\nCMD PORT %d\n\n", NOTGIOS_MONITOR_PORT);
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
      if (retval >= 0) {
        e_count = 0;
        actual += retval;
      } else if (e_count++ > 5) {
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
  int actual = 0, expected = strlen(buffer) + 1;
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
int create_server() {
  int server_fd;
  struct sockaddr_in serv_addr;
  server_fd = socket(AF_INET, SOCK_STREAM, 0);
  fcntl(server_fd, F_SETFL, O_NONBLOCK);
  if (server_fd < 0) return NOTGIOS_SOCKET_FAILURE;
  memset(&serv_addr, 0, sizeof(serv_addr));
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_addr.s_addr = INADDR_ANY;
  serv_addr.sin_port = htons(NOTGIOS_MONITOR_PORT);
  if (bind(server_fd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) return NOTGIOS_SOCKET_FAILURE;
  listen(server_fd, 10);
  return server_fd;
}

thread_control_t *create_thread_control() {
  thread_control_t *control = malloc(sizeof(thread_control_t));

  if (control) {
    control->paused = 0;
    control->killed = 0;
    pthread_cond_init(&control->signal, NULL);
    pthread_mutex_init(&control->mutex, NULL);
  }

  return control;
}

void destroy_thread_control(void *voidarg) {
  thread_control_t *control = voidarg;
  if (control) {
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
  } while (current = strtok(NULL, "\n"));
  return NOTGIOS_SUCCESS;
}

void user_error() {
  fprintf(stderr, "This utility is used internally by the Notgios host monitoring framework, and is not meant to be launched manually.\n");
  exit(EINVAL);
}
