/*----- System Includes -----*/

#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <errno.h>
#include <syslog.h>

/*----- Local Includes -----*/

#include "../include/hash.h"

/*----- Constant Declarations -----*/

#define NOTGIOS_MONITOR_PORT 31089
#define NOTGIOS_ACCEPT_TIMEOUT 60
#define NOTGIOS_READ_TIMEOUT 20
#define NOTGIOS_WRITE_TIMEOUT 4
#define NOTGIOS_STATIC_BUFSIZE 256
#define NOTGIOS_MAX_COMMANDS 16
#define NOTGIOS_MAX_OPTIONS 4
#define NOTGIOS_MAX_OPTION_LEN 16
#define NOTGIOS_MAX_TYPE_LEN 16
#define NOTGIOS_MAX_METRIC_LEN 8
#define NOTGIOS_MAX_NUM_LEN 12
#define NOTGIOS_SUCCESS 0x0
#define NOTGIOS_GENERIC_ERROR -0x01
#define NOTGIOS_BAD_HOSTNAME -0x02
#define NOTGIOS_SERVER_UNREACHABLE -0x04
#define NOTGIOS_SERVER_REJECTED -0x08
#define NOTGIOS_SOCKET_FAILURE -0x10
#define NOTGIOS_SOCKET_CLOSED -0x20
#define NOTGIOS_TOO_MANY_ARGS -0x40

/*----- Macro Declarations -----*/

#define RETURN_NACK(buf, msg)                                       \
  do {                                                              \
    sprintf(buf, msg);                                              \
    return;                                                         \
  } while (0);

/*----- Function Declarations -----*/

void *launch_worker_thread(void *args);
void handle_add(char **commands, char *reply_buf, hash_t *threads);
void handle_pause(char **commands, char *reply_buf);
void handle_resume(char **commands, char *reply_buf);
void handle_delete(char **commands, char *reply_buf);

int create_server();
int handshake(char *server_hostname, int port, int initial);
int handle_read(int fd, char *buffer, int len);
int handle_write(int fd, char *buffer);
int parse_commands(char **output, char *input);
void user_error();

/*----- Type Declarations -----*/

typedef enum {
  PROCESS,
  DIRECTORY,
  DISK,
  SWAP,
  LOAD
} task_type_t;

typedef enum {
  MEMORY,
  CPU,
  IO,
  NONE
} metric_type_t;

typedef char thread_option_t[NOTGIOS_MAX_OPTION_LEN];

typedef struct thread_args {
  int id, freq;
  task_type_t type;
  metric_type_t metric;
  thread_option_t options[NOTGIOS_MAX_OPTIONS];
} thread_args_t;

/*----- Evil but Necessary Globals -----*/

int connected = 0;

/*----- Function Implementations -----*/

int main(int argc, char **argv) {
  int c, port = 0, server_socket = 0, initial = 1;
  char *server_hostname = NULL;

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
  openlog("Notgios Monitor", 0, 0);
  hash_t threads;
  init_hash(&threads, free);

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
    connected = 1;

    // We're connected. Start reading jobs and stuff from the server.
    while (1) {
      int retval = handle_read(socket, buffer, NOTGIOS_STATIC_BUFSIZE);
      if (retval > 0) {
        char *commands[NOTGIOS_MAX_COMMANDS];
        memset(buffer, 0, sizeof(char) * NOTGIOS_STATIC_BUFSIZE);

        if (parse_commands(commands, buffer)) {
          char *cmd = commands[0];
          if (strstr(cmd, "NGS JOB ADD") == cmd) {
            handle_add(commands, buffer, &threads);
          } else if (strstr(cmd, "NGS JOB PAUS") == cmd) {
            handle_pause(commands, buffer);
          } else if (strstr(cmd, "NGS JOB RES") == cmd) {
            handle_resume(commands, buffer);
          } else if (strstr(cmd, "NGS JOB DEL") == cmd) {
            handle_delete(commands, buffer);
          } else if (strstr(cmd, "NGS STILL THERE?") == cmd) {
            // Manual keepalive. I know TCP is supposed to do stuff like this on its own, but honestly it makes
            // it easier on my end to detect errors if I also do it manually.
            sprintf(buffer, "NGS STILL HERE!\n\n");
          } else if (strstr(cmd, "NGS BYE")) {

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

    // Either our socket has been unexpectedly closed (server crashed), or we received an orderly
    // shutdown message from the server (server was interrupted by user, or machine server was
    // running on was shutdown). Either way, we need to start buffering output and attempt to
    // reopen the connection with the server.
    // TODO: Actual cleanup logic and stuff. Need to read up on socket behavior in case of
    // unexpected shutdown.
    connected = 0;
    close(socket);
    close(server_socket);
  }
}

void handle_add(char **commands, char *reply_buf, hash_t *threads) {
  int id, freq;
  char type_str[NOTGIOS_MAX_TYPE_LEN], metric_str[NOTGIOS_MAX_METRIC_LEN], id_str[NOTGIOS_MAX_NUM_LEN];
  task_type_t type;
  metric_type_t metric;

  // Pull out the parameters.
  sscanf(commands[1], "ID %d", &id);
  sscanf(commands[1], "ID %s", id_str);
  sscanf(commands[2], "TYPE %s", type_str);
  sscanf(commands[3], "METRIC %s", metric_str);
  sscanf(commands[4], "FREQ %d", &freq);

  // "Convert" from string to enum value.
  if (!strcmp(type_str, "PROCESS")) type = PROCESS;
  else if (!strcmp(type_str, "DIRECTORY")) type = DIRECTORY;
  else if (!strcmp(type_str, "DISK")) type = DISK;
  else if (!strcmp(type_str, "SWAP")) type = SWAP;
  else if (!strcmp(type_str, "LOAD")) type = LOAD;
  else RETURN_NACK(reply_buf, "NGS NACK\nCAUSE UNRECOGNIZED_TYPE\n\n");

  // "Convert" from string to enum value.
  if (!strcmp(metric_str, "MEMORY")) metric = MEMORY;
  else if (!strcmp(metric_str, "CPU")) metric = CPU;
  else if (!strcmp(metric_str, "IO")) metric = IO;
  else if (!strcmp(metric_str, "NONE")) metric = NONE;
  else RETURN_NACK(reply_buf, "NGS NACK\nCAUSE UNRECOGNIZED_METRIC\n\n");

  // Get information ready to pass onto the thread.
  thread_args_t *arguments = calloc(1, sizeof(thread_args_t));
  arguments->id = id;
  arguments->freq = freq;
  arguments->type = type;
  arguments->metric = metric;

  // Look over rest of commands if applicable.
  if (type == PROCESS || type == DISK) {
    int elem = 0;
    char *error = "NGS NACK\nCAUSE INAPPLICABLE_OPTION\n\n";

    // This is really too long, but I love that I can do all of this on one line, so I'm leaving it on one line.
    for (char **cmd_ptr = commands + 5, *cmd = *cmd_ptr; *cmd_ptr && cmd_ptr - commands < NOTGIOS_MAX_COMMANDS; cmd_ptr++, cmd = *cmd_ptr) {
      // The structure of these if statements makes me sad. Maybe I'll refactor them sometime.
      if (strstr("KEEPALIVE", cmd) == cmd || strstr("PIDFILE", cmd) == cmd || strstr("RUNCMD", cmd) == cmd) {
        if (type != PROCESS) {
          free(arguments);
          RETURN_NACK(reply_buf, error);
        }
      } else if (strstr("MNTPNT", cmd) == cmd) {
        if (type != DISK) {
          free(arguments);
          RETURN_NACK(reply_buf, error);
        }
      } else {
        free(arguments);
        RETURN_NACK(reply_buf, "NGS NACK\nCAUSE UNRECOGNIZED_OPTION\n\n");
      }

      memcpy(arguments->options[elem++], cmd, strlen(cmd) + 1);
    }
  }

  // Create a new thread to run the task!
  pthread_t *task = malloc(sizeof(pthread_t));
  pthread_create(task, NULL, launch_worker_thread, arguments);
  hash_put(threads, id_str, task);
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

int parse_commands(char **output, char *input) {
  int elem = 0;
  memset(output, 0, sizeof(char *) * NOTGIOS_MAX_COMMANDS);
  char *current = strtok(input, "\n");
  do {
    if (elem == 16) return NOTGIOS_TOO_MANY_ARGS;
    output[elem++] = current;
  } while (current = strtok(NULL, "\n"));
  return NOTGIOS_SUCCESS;
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
    struct timeval time;
    time.tv_sec = NOTGIOS_READ_TIMEOUT;
    time.tv_usec = 0;

    select(fd + 1, &to_read, NULL, NULL, &time);
    if (FD_ISSET(fd, &to_read)) {
      int retval = read(fd, buffer, len);
      if (retval >= 0) {
        e_count = 0;
        actual += retval;
      } else if (e_count++ > 5) {
        return NOTGIOS_SOCKET_CLOSED;
      }
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

void user_error() {
  fprintf(stderr, "This utility is used internally by the Notgios host monitoring framework, and is not meant to be launched manually.\n");
  exit(EINVAL);
}
