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
#include "../include/list.h"

/*----- Local Function Declaractions -----*/

// Collection Type Handlers
int handle_process(metric_type_t metric, task_option_t *options, char *id);
int handle_directory(task_option_t *options, char *id);
int handle_disk(metric_type_t metric, task_option_t *options, char *id);
int handle_swap(char *id);
int handle_load(char *id);
int handle_total(char *id, metric_type_t metric);

// Collection Functions
int process_memory_collect(uint16_t pid, task_report_t *data);
int process_cpu_collect(uint16_t pid, task_report_t *data);
int process_io_collect(uint16_t pid, task_report_t *data);
int directory_memory_collect(uint16_t pid, task_report_t *data);
int disk_memory_collect(uint16_t pid, task_report_t *data);
int swap_collect(uint16_t pid, task_report_t *data);
int load_collect(uint16_t pid, task_report_t *data);
int total_memory_collect(task_report_t *data);
int total_cpu_collect(task_report_t *data);
int total_io_collect(task_report_t *data);

// Utility Functions
int check_statm();
void init_task_report(task_report_t *report, task_type_t type, metric_type_t metric);

/*----- Evil but Necessary Globals -----*/

extern hash_t threads, controls, children;
extern list_t reports;
extern pthread_rwlock_t connection_lock;
extern int connected, exiting;

/*----- Function Implementations -----*/

int run_task(task_type_t type, metric_type_t metric, task_option_t *options, char *id) {
  switch (type) {
    case PROCESS:
      return handle_process(metric, options, id);
    case DIRECTORY:
      return handle_directory(options, id);
    case DISK:
      return handle_disk(metric, options, id);
    case SWAP:
      return handle_swap(id);
    case LOAD:
      return handle_load(id);
    case TOTAL:
      return handle_total(id, metric);
  }
}

int handle_process(metric_type_t metric, task_option_t *options, char *id) {
  int keepalive = 0;
  uint16_t pid;
  char *pidfile, *runcmd;
  task_report_t report;
  init_task_report(&report, PROCESS, metric);

  // Pull out options.
  for (int i = 0; i < NOTGIOS_MAX_OPTIONS; i++) {
    task_option_t *option = &options[i];
    switch (option->type) {
      case KEEPALIVE:
        keepalive = strcmp(option->value, "TRUE") ? 0 : 1;
        break;
      case PIDFILE:
        pidfile = option->value;
        break;
      case RUNCMD:
        runcmd = option->value;
        break;
      case EMPTY:
        break;
    }
  }
  write_log(LOG_DEBUG, "Task %s: Finished parsing arguments for task...\n", id);

  // Figure out process running/not running situation.
  if (keepalive) {
    FILE *file = fopen(pidfile, "w+");
    if (!file) {
      // We cannot write to the given pidfile path. Most likely the directory just
      // doesn't exist, but I'm defining this as an unrecoverable error, so send a message
      // to the frontend and remove the task.
      write_log(LOG_ERR, "Task %s: Pidfile inaccessible for keepalive process...\n", id);
      sprintf(report.message, "FATAL CAUSE NO_PIDFILE");
      lpush(&reports,  &report);
      return NOTGIOS_TASK_FATAL;
    }
    write_log(LOG_DEBUG, "Task %s: Successfully opened pidfile for keepalive process...\n", id);

    int *tmp_pid = hash_get(&children, id);
    if (tmp_pid) {
      write_log(LOG_DEBUG, "Task %s: Keepalive Process is already running...\n", id);
      pid = *tmp_pid;
      fprintf(file, "%u", pid);
    } else {
      pid = fork();
      if (pid) {
        write_log(LOG_DEBUG, "Task %s: Forked...\n", id);
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
        while ((arg = strtok(NULL, "\t")) && elem < NOTGIOS_MAX_ARGS) args[elem++] = arg;

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
      write_log(LOG_DEBUG, "Task %s: Got pid for watched process...\n", id);

      if (retval && retval != EOF) {
        // We read the pid successfully.
        retval = kill(other_pid, 0);
        if (!retval) {
          // The process is running!
          write_log(LOG_DEBUG, "Task %s: Watched process is still running...\n", id);
          pid = other_pid;
        } else {
          // The process is not currently running, enqueue a report saying this, then return.
          write_log(LOG_ERR, "Task %s: Kill revealed watched process is not running...\n", id);
          sprintf(report.message, "ERROR CAUSE PROC_NOT_RUNNING");
          lpush(&reports, &report);
          return NOTGIOS_SUCCESS;
        }
      } else {
        // The process is not currently running, enqueue a report saying this, then return.
        write_log(LOG_ERR, "Task %s: Pidfile not formatted correctly for watched process...\n", id);
        sprintf(report.message, "ERROR CAUSE PROC_NOT_RUNNING");
        lpush(&reports, &report);
        return NOTGIOS_SUCCESS;
      }
    } else {
      // We can't access the file. I'm defining this as an unrecoverable error, so
      // send a message to the frontend, and then remove the task.
      write_log(LOG_ERR, "Task %s: Pidfile not accessible for watched process...\n", id);
      sprintf(report.message, "FATAL CAUSE NO_PIDFILE");
      lpush(&reports, &report);
      return NOTGIOS_TASK_FATAL;
    }
  }

  // Collect metrics.
  int retval;
  switch (metric) {
    case MEMORY:
      retval = process_memory_collect(pid, &report);
      if (retval == NOTGIOS_NOPROC && keepalive) {
        if (check_statm()) {
          // FIXME: This was written before the child handler was figured out. Could need to revisit this
          // after implementing the child handler.
          // Since we're keeping alive, this most likely means that the user gave us an invalid command.
          // Send an error message, and the frontend will eventually kill the task if necessary.
          write_log(LOG_ERR, "Task %s: Watched/Keepalive process is not running for collection...\n", id);
          sprintf(report.message, "ERROR CAUSE PROC_NOT_RUNNING");
        } else {
          // We can't even read from /proc/self/statm, which should be guaranteed to work on any version of
          // linux that supports statm at all. Let the front end know that we're running on an unsupported
          // distro.
          write_log(LOG_ERR, "Task %s: Running on unsupported distro...\n", id);
          sprintf(report.message, "FATAL CAUSE UNSUPPORTED_DISTRO");
          return NOTGIOS_TASK_FATAL;
        } 
      } else {
        write_log(LOG_DEBUG, "Task %s: Memory info colleted...\n", id);
      }
      break;
    case CPU:
      retval = process_cpu_collect(pid, &report);
      break;
    case IO:
      retval = process_io_collect(pid, &report);
  }

  // Enqueue metrics for sending.
  write_log(LOG_DEBUG, "Task %s: Enqueuing report and returning...\n", id);
  lpush(&reports, &report);
  return NOTGIOS_SUCCESS;
}

int handle_directory(task_option_t *options, char *id) {
  // TODO: Write this function.
}

int handle_disk(metric_type_t metric, task_option_t *options, char *id) {
  // TODO: Write this function.
}

int handle_swap(char *id) {
  // TODO: Write this function.
}

int handle_load(char *id) {
  // TODO: Write this function.
}

int handle_total(char *id, metric_type_t metric) {
  // TODO: Write this function.
}

int process_memory_collect(uint16_t pid, task_report_t *data) {
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

int process_cpu_collect(uint16_t pid, task_report_t *data) {
  // TODO: Write this function.
}

int process_io_collect(uint16_t pid, task_report_t *data) {
  // TODO: Write this function.
}

int directory_memory_collect(uint16_t pid, task_report_t *data) {
  // TODO: Write this function.
}

int disk_memory_collect(uint16_t pid, task_report_t *data) {
  // TODO: Write this function.
}

int swap_collect(uint16_t pid, task_report_t *data) {
  // TODO: Write this function.
}

int load_collect(uint16_t pid, task_report_t *data) {
  // TODO: Write this function.
}

int total_memory_collect(task_report_t *data) {
  // TODO: Write this function.
}

int total_cpu_collect(task_report_t *data) {
  // TODO: Write this function.
}

int total_io_collect(task_report_t *data) {
  // TODO: Write this function.
}

int check_statm() {
  return !access("/proc/self/statm", F_OK);
}

void init_task_report(task_report_t *report, task_type_t type, metric_type_t metric) {
  if (report) {
    memset(report->id, 0, sizeof(char) * NOTGIOS_MAX_NUM_LEN);
    memset(report->message, 0, sizeof(char) * NOTGIOS_ERROR_BUFSIZE);
    report->percentage = 0;
    report->value = 0;
    report->type = EMPTY;
    report->metric = NONE;
  }
}
