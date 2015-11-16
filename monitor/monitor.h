#ifndef MONITOR_H
#define MONITOR_H

/*----- System Includes -----*/

#include <pthread.h>

/*----- Constant Declarations -----*/

#define NOTGIOS_MONITOR_PORT 31089
#define NOTGIOS_ACCEPT_TIMEOUT 60
#define NOTGIOS_READ_TIMEOUT 20
#define NOTGIOS_WRITE_TIMEOUT 4
#define NOTGIOS_STATIC_BUFSIZE 512
#define NOTGIOS_ERROR_BUFSIZE 64
#define NOTGIOS_REQUIRED_COMMANDS 5
#define NOTGIOS_MAX_OPTIONS 4
#define NOTGIOS_MAX_OPTION_LEN 16
#define NOTGIOS_MAX_TYPE_LEN 16
#define NOTGIOS_MAX_METRIC_LEN 8
#define NOTGIOS_MAX_NUM_LEN 12
#define NOTGIOS_MAX_ARGS 32
#define NOTGIOS_MAX_PROC_LEN 32
#define NOTGIOS_SUCCESS 0x0
#define NOTGIOS_GENERIC_ERROR -0x01
#define NOTGIOS_BAD_HOSTNAME -0x02
#define NOTGIOS_SERVER_UNREACHABLE -0x04
#define NOTGIOS_SERVER_REJECTED -0x08
#define NOTGIOS_SOCKET_FAILURE -0x10
#define NOTGIOS_SOCKET_CLOSED -0x20
#define NOTGIOS_TOO_MANY_ARGS -0x40
#define NOTGIOS_EXEC_FAILED -0x80

/*----- Type Declaractions -----*/

typedef enum {
  PROCESS,
  DIRECTORY,
  DISK,
  SWAP,
  LOAD,
  TOTAL
} task_type_t;

typedef enum {
  MEMORY,
  CPU,
  IO,
  NONE
} metric_type_t;

typedef enum {
  KEEPALIVE,
  PIDFILE,
  RUNCMD,
  MNTPNT,
  PATH
} task_option_type_t;

typedef enum {
  PAUSE,
  RESUME,
  DELETE
} task_action_t;

typedef struct task_option {
  task_option_type_t type;
  char value[NOTGIOS_MAX_OPTION_LEN];
} task_option_t;

typedef struct thread_control {
  int paused, killed, dropped;
  pthread_cond_t signal;
  pthread_mutex_t mutex;
} thread_control_t;

typedef struct thread_args {
  int freq;
  char id[NOTGIOS_MAX_NUM_LEN];
  task_type_t type;
  metric_type_t metric;
  thread_control_t *control;
  task_option_t options[NOTGIOS_MAX_OPTIONS];
} thread_args_t;

#endif