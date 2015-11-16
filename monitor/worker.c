/*----- System Includes -----*/

#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <syslog.h>
#include <pthread.h>

/*----- Local Includes -----*/

#include "worker.h"
#include "../include/hash.h"

/*----- Local Function Declaractions -----*/

// Collection Type Handlers
void handle_process(metric_type_t metric, task_option_t *options, char *id);
void handle_directory(task_option_t *options, char *id);
void handle_disk(metric_type_t metric, task_option_t *options, char *id);
void handle_swap(char *id);
void handle_load(char *id);
void handle_total(char *id, metric_type_t metric);

// Collection Functions
int process_memory_collect(uint16_t pid, task_data_t *data);
int process_cpu_collect(uint16_t pid, task_data_t *data);
int process_io_collect(uint16_t pid, task_data_t *data);
int directory_memory_collect(uint16_t pid, task_data_t *data);
int disk_memory_collect(uint16_t pid, task_data_t *data);
int swap_collect(uint16_t pid, task_data_t *data);
int load_collect(uint16_t pid, task_data_t *data);
int total_memory_collect(task_data_t *data);
int total_cpu_collect(task_data_t *data);
int total_io_collect(task_data_t *data);

// Utility Functions
int check_statm();
void init_task_data(task_data_t *data);

/*----- Evil but Necessary Globals -----*/

extern hash_t threads, controls, children;
extern pthread_rwlock_t connection_lock;
extern int connected, exiting;

/*----- Function Implementations -----*/

void run_task(task_type_t type, metric_type_t metric, task_option_t *options, char *id) {
  switch (type) {
    case PROCESS:
      handle_process(metric, options, id);
      break;
    case DIRECTORY:
      handle_directory(options, id);
      break;
    case DISK:
      handle_disk(metric, options, id);
      break;
    case SWAP:
      handle_swap(id);
      break;
    case LOAD:
      handle_load(id);
      break;
    case TOTAL:
      handle_total(id, metric);
  }
}

void handle_process(metric_type_t metric, task_option_t *options, char *id) {
  int keepalive = 0;
  uint16_t pid;
  char *pidfile, *runcmd;
  task_data_t data;
  init_task_data(&data);

  // Pull out options.
  for (int i = 0; i < NOTGIOS_MAX_OPTIONS; i++) {
    task_option_t option = options[i];
    switch (option.type) {
      case KEEPALIVE:
        keepalive = strcmp(option.value, "TRUE") ? 0 : 1;
        break;
      case PIDFILE:
        pidfile = option.value;
        break;
      case RUNCMD:
        runcmd = option.value;
    }
  }

  // Figure out process running/not running situation.
  if (keepalive) {
    FILE *file = fopen(pidfile, "w+");
    if (!file) {
      // TODO: We cannot write to the given pidfile path. Most likely the directory just
      // doesn't exist, but I'm defining this as an unrecoverable error, so send a message
      // to the frontend and remove the task.
      sprintf(data.message, "FATAL CAUSE NO_PIDFILE");
    }

    pid = *(int *) hash_get(&children, id);
    if (pid) {
      fprintf(file, "%u", pid);
    } else {
      pid = fork();
      if (pid) {
        uint16_t *pid_cpy = malloc(sizeof(uint16_t));
        *pid_cpy = pid;
        hash_put(&children, id, pid_cpy);
        fprintf(file, "%u", pid);
      } else {
        int elem = 0;
        char *args[NOTGIOS_MAX_ARGS];
        memset(args, 0, sizeof(char *) * NOTGIOS_MAX_ARGS);

        // Just sleep for a tiny bit to make sure our pid got into the children table.
        // No validation has been done on the run command, so, odds are, it won't work
        // and exec will fail. If so, need to make sure our pid is in the children table
        // so that the SIGCHLD handler will know what to do with our exit status.
        sleep(0.1);

        // Get our base command and arguments.
        char *path = strtok(runcmd, "\t"), *arg;
        while ((arg = strtok(NULL, "\n")) && elem < NOTGIOS_MAX_ARGS) args[elem++] = arg;

        // Moment of truth!
        execv(path, args);
        exit(NOTGIOS_EXEC_FAILED);
      }
    }
    fclose(file);
  } else {
    uint16_t other_pid;
    FILE *file = fopen(pidfile, "r");
    if (file) {
      // We can access the file.
      int retval = fscanf(file, "%u", &other_pid);
      fclose(file);

      if (retval && retval != EOF) {
        // We read the pid successfully.
        retval = kill(other_pid, 0);
        if (!retval) {
          // The process is running!
          pid = other_pid;
        } else {
          // TODO: The process is not currently running. We can't collect any metrics
          // unless it's running. Send a message to the front end letting it know the
          // process isn't running, then return.
          sprintf(data.message, "ERROR CAUSE PROC_NOT_RUNNING");
        }
      } else {
        // TODO: The process is not currently running. We can't collect any metrics
        // unless it's running. Send a message to the front end letting it know the
        // process isn't running, then return.
        sprintf(data.message, "ERROR CAUSE PROC_NOT_RUNNING");
      }
    } else {
      // TODO: We can't access the file. I'm defining this as an unrecoverable error, so
      // send a message to the frontend, and then remove the task.
      sprintf(data.message, "FATAL CAUSE NO_PIDFILE");
    }
  }

  // Collect Metrics.
  int retval;
  switch (metric) {
    case MEMORY:
      retval = process_memory_collect(pid, &data);
      if (retval == NOTGIOS_NOPROC && keepalive) {
        if (check_statm()) sprintf(data.message, "ERROR CAUSE PROC_NOT_RUNNING");
        else sprintf(data.message, "FATAL CAUSE UNSUPPORTED_DISTRO");
      }
      break;
    case CPU:
      retval = process_cpu_collect(pid, &data);
      break;
    case IO:
      retval = process_io_collect(pid, &data);
  }

  // TODO: Enqueue metrics for sending.
}

void handle_directory(task_option_t *options, char *id) {
  // TODO: Write this function.
}

void handle_disk(metric_type_t metric, task_option_t *options, char *id) {
  // TODO: Write this function.
}

void handle_swap(char *id) {
  // TODO: Write this function.
}

void handle_load(char *id) {
  // TODO: Write this function.
}

void handle_total(char *id, metric_type_t metric) {
  // TODO: Write this function.
}

int process_memory_collect(uint16_t pid, task_data_t *data) {
  char path[NOTGIOS_MAX_PROC_LEN];
  sprintf(path, "/proc/%u/statm", pid);
  FILE *statm = fopen(path, "r");
  if (statm) {
    long usage;
    fscanf(statm, "%ld", &usage);
    data->value = (double) usage;
    return NOTGIOS_SUCCESS;
  } else {
    // Can't open memory file. Return error to our calling function and let it figure things out.
    return NOTGIOS_NOPROC;
  }
}

int process_cpu_collect(uint16_t pid, task_data_t *data) {
  // TODO: Write this function.
}

int process_io_collect(uint16_t pid, task_data_t *data) {
  // TODO: Write this function.
}

int directory_memory_collect(uint16_t pid, task_data_t *data) {
  // TODO: Write this function.
}

int disk_memory_collect(uint16_t pid, task_data_t *data) {
  // TODO: Write this function.
}

int swap_collect(uint16_t pid, task_data_t *data) {
  // TODO: Write this function.
}

int load_collect(uint16_t pid, task_data_t *data) {
  // TODO: Write this function.
}

int total_memory_collect(task_data_t *data) {
  // TODO: Write this function.
}

int total_cpu_collect(task_data_t *data) {
  // TODO: Write this function.
}

int total_io_collect(task_data_t *data) {
  // TODO: Write this function.
}

int check_statm() {
  return !access("/proc/self/statm", F_OK);
}

void init_task_data(task_data_t *data) {
  if (data) {
    memset(data->id, 0, sizeof(char) * NOTGIOS_MAX_NUM_LEN);
    memset(data->message, 0, sizeof(char) * NOTGIOS_ERROR_BUFSIZE);
    data->percentage = 0;
    data->value = 0;
  }
}
