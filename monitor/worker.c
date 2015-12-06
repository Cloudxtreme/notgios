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
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <dirent.h>

/*----- Local Includes -----*/

#include "worker.h"
#include "../include/hash.h"
#include "../include/list.h"

/*----- Macro Declarations -----*/

#define RETURN_UNSUPPORTED_DISTRO(report, id)                                 \
  do {                                                                        \
    write_log(LOG_ERR, "Task %s: Running on an unsupported distro...\n", id); \
    sprintf(report.message, "FATAL CAUSE UNSUPPORTED_DISTRO");                \
    lpush(&reports, &report);                                                 \
    return NOTGIOS_TASK_FATAL;                                                \
  } while (0);

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
long directory_memory_collect(char *path);
int disk_memory_collect(uint16_t pid, task_report_t *data);
int disk_io_collect(uint16_t pid, task_report_t *data);
int swap_collect(uint16_t pid, task_report_t *data);
int load_collect(uint16_t pid, task_report_t *data);
int total_memory_collect(task_report_t *data);
int total_cpu_collect(task_report_t *data);
int total_io_collect(task_report_t *data);

// Utility Functions
int check_statm();
int check_stat();
void init_task_report(task_report_t *report, char *id, task_type_t type, metric_type_t metric);

/*----- Evil but Necessary Globals -----*/

extern hash_t threads, controls, children;
extern list_t reports;
extern monitor_stats_t task_stats;
extern pthread_rwlock_t stats_lock;

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
    default: {
      // We've been passed an incorrectly initialized task. Shouldn't happen, but handle
      // for debugging. Plus it gets GCC off my case.
      task_report_t report;
      init_task_report(&report, id, type, metric);
      sprintf(report.message, "FATAL CAUSE INVALID_TASK");
      lpush(&reports, &report);
      return NOTGIOS_GENERIC_ERROR;
    }
  }
}

int handle_process(metric_type_t metric, task_option_t *options, char *id) {
  int keepalive = 0;
  uint16_t pid;
  char *pidfile, *runcmd;
  task_report_t report;
  init_task_report(&report, id, PROCESS, metric);

  // Pull out options.
  // FIXME: Need to go over this again to make sure all necessary validation of parameters
  // is performed.
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
        // User chose not to specify an option. This is fine, move on.
        break;
      default:
        // We've been passed a task containing invalid options. Shouldn't happen, but handle
        // it for debugging.
        sprintf(report.message, "FATAL CAUSE INVALID_TASK");
        lpush(&reports, &report);
        return NOTGIOS_GENERIC_ERROR;
    }
  }
  write_log(LOG_DEBUG, "Task %s: Finished parsing arguments for process task...\n", id);

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

    uint16_t *tmp_pid = hash_get(&children, id);
    if (tmp_pid) {
      write_log(LOG_DEBUG, "Task %s: Keepalive Process is already running...\n", id);
      pid = *tmp_pid;
      fprintf(file, "%hu", pid);
    } else {
      pid = fork();
      if (pid) {
        write_log(LOG_DEBUG, "Task %s: Forked...\n", id);
        uint16_t *pid_cpy = malloc(sizeof(uint16_t));
        *pid_cpy = pid;

        // Put the new pid into the children hash, update the pidfile, and just make
        // sure we aren't doing all of this for nothing.
        int retval = hash_put(&children, id, pid_cpy);
        fprintf(file, "%hu", pid);
        if (retval == HASH_FROZEN) return NOTGIOS_IN_SHUTDOWN;
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
      int retval = fscanf(file, "%hu", &other_pid);
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
          RETURN_UNSUPPORTED_DISTRO(report, id);
        } 
      } else {
        write_log(LOG_DEBUG, "Task %s: Memory info collected...\n", id);
      }
      break;
    case CPU:
      retval = process_cpu_collect(pid, &report);
      if (retval == NOTGIOS_NOPROC) {
        if (check_stat()) {
          write_log(LOG_ERR, "Task %s: Watched/Keepalive process is not running for collection...\n", id);
          sprintf(report.message, "ERROR CAUSE PROC_NOT_RUNNING");
        } else {
          // We can't even read from /proc/self/stat, which means it either doesn't exist, or we don't
          // support the format it's using. Either way, we're done.
          RETURN_UNSUPPORTED_DISTRO(report, id);
        }
      } else if (retval == NOTGIOS_UNSUPP_DISTRO) {
        // Collection function encountered an error condition that suggests we're running on an
        // unsupported distro.
        RETURN_UNSUPPORTED_DISTRO(report, id);
      } else {
        write_log(LOG_DEBUG, "Task %s: CPU Time collected...\n", id);
      }
      break;
    case IO:
      // This is currently unimplemented.
      retval = process_io_collect(pid, &report);
      break;
    default:
      // We've been passed a task containing invalid options. Shouldn't happen, but handle it
      // for debugging.
      sprintf(report.message, "FATAL CAUSE INVALID_TASK");
      lpush(&reports, &report);
      return NOTGIOS_GENERIC_ERROR;
  }

  if (retval == NOTGIOS_UNSUPP_TASK) {
    write_log(LOG_DEBUG, "Task %s: Received an unsupported task. Removing...");
    sprintf(report.message, "FATAL CAUSE UNSUPPORTED_TASK");
    lpush(&reports, &report);
    return NOTGIOS_TASK_FATAL;
  }

  // Enqueue metrics for sending.
  write_log(LOG_DEBUG, "Task %s: Enqueuing report and returning...\n", id);
  lpush(&reports, &report);
  return NOTGIOS_SUCCESS;
}

int handle_directory(task_option_t *options, char *id) {
  char *path = NULL;
  task_report_t report;
  init_task_report(&report, id, DIRECTORY, MEMORY);

  // Pull out options
  for (int i = 0; i < NOTGIOS_MAX_OPTIONS; i++) {
    task_option_t *option = &options[i];
    switch (option->type) {
      case PATH:
        path = option->value;
        break;
      case EMPTY:
        // There's only one option for directory tasks, so this will be the most common branch.
        break;
      default:
        // We've been passed a task containing invalid options. Shouldn't happen, but handle
        // it for debugging.
        sprintf(report.message, "FATAL CAUSE INVALID_TASK");
        lpush(&reports, &report);
        return NOTGIOS_GENERIC_ERROR;
    }
  }
  write_log(LOG_DEBUG, "Task %s: Finished parsing arguments for directory task...\n", id);

  // Perform some error handling on the given path.
  if (!path) {
    // The server should take care of making sure this doesn't happen, but the directory option
    // wasn't sent.
    write_log(LOG_ERR, "Task %s: Recevied directory task with no path option...\n", id);
    sprintf(report.message, "FATAL CAUSE TASK_MISSING_OPTIONS");
    lpush(&reports, &report);
    return NOTGIOS_TASK_FATAL;
  } else if (access(path, F_OK)) {
    // We can't access the directory for some reason.
    write_log(LOG_ERR, "Task %s: Cannot access directory...\n", id);
    if (errno == EACCES || errno == ENOENT) sprintf(report.message, "FATAL CAUSE DIR_NOT_ACCESSIBLE");
    else if (errno == ELOOP) sprintf(report.message, "FATAL CAUSE DIR_INFINITE_LOOP");
    else if (errno == ENAMETOOLONG) sprintf(report.message, "FATAL CAUSE DIR_NAME_TOO_LONG");
    else sprintf(report.message, "FATAL CAUSE UNKNOWN");
    lpush(&reports, &report);
    return NOTGIOS_TASK_FATAL;
  }

  // Recursively calculate the directory size.
  write_log(LOG_DEBUG, "Task %s: Calculating directory size...\n", id);
  long retval = directory_memory_collect(path);
  if (retval >= 0) {
    report.value = (double) retval;
    report.time_taken = time(NULL);
  } else if (retval == NOTGIOS_BAD_ACCESS) {
    write_log(LOG_ERR, "Task %s: Access was refused for a subdirectory...\n", id);
    sprintf(report.message, "FATAL CAUSE SUBDIR_NOT_ACCESSIBLE");
    lpush(&reports, &report);
    return NOTGIOS_TASK_FATAL;
  } else if (retval == NOTGIOS_NO_FILES) {
    write_log(LOG_ERR, "Task %s: Failed to open a file due to too many files being open...\n", id);
    sprintf(report.message, "ERROR CAUSE TOO_MANY_FILES");
  }

  // Enqueue metrics for sending.
  write_log(LOG_DEBUG, "Task %s: Enqueuing report and returning...\n", id);
  lpush(&reports, &report);
  return NOTGIOS_SUCCESS;
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
  task_report_t report;
  init_task_report(&report, id, PROCESS, metric);

  int retval;
  switch (metric) {
    case MEMORY:
      retval = total_memory_collect(&report);
      if (retval == NOTGIOS_UNSUPP_DISTRO) RETURN_UNSUPPORTED_DISTRO(report, id);
      write_log(LOG_DEBUG, "Task %s: Total memory info collected...\n", id);
      break;
    case CPU:
      retval = total_cpu_collect(&report);
      if (retval == NOTGIOS_UNSUPP_DISTRO) RETURN_UNSUPPORTED_DISTRO(report, id);
      write_log(LOG_DEBUG, "Task %s: Total CPU usage collected...\n", id);
      break;
    case IO:
      // This is currently unimplemented.
      retval = total_io_collect(&report);
      break;
    default:
      // We've been passed a task containing invalid options. Shouldn't happen, but handle it
      // for debugging.
      sprintf(report.message, "FATAL CAUSE INVALID_TASK");
      lpush(&reports, &report);
      return NOTGIOS_GENERIC_ERROR;
  }

  if (retval == NOTGIOS_UNSUPP_TASK) {
    write_log(LOG_DEBUG, "Task %s: Received an unsupported task. Removing...\n", id);
    sprintf(report.message, "FATAL CAUSE UNSUPPORTED_TASK");
    lpush(&reports, &report);
    return NOTGIOS_TASK_FATAL;
  }

  write_log(LOG_DEBUG, "Task %s: Enqueuing report an returning...\n", id);
  lpush(&reports, &report);
  return NOTGIOS_SUCCESS;
}

int process_memory_collect(uint16_t pid, task_report_t *data) {
  char path[NOTGIOS_MAX_PROC_LEN];
  sprintf(path, "/proc/%hu/statm", pid);
  FILE *statm = fopen(path, "r");
  if (statm) {
    long usage;
    int retval = fscanf(statm, "%ld", &usage);
    if (retval == 1) {
      data->value = (double) usage;
      data->time_taken = time(NULL);
      return NOTGIOS_SUCCESS;
    } else {
      return NOTGIOS_NOPROC;
    }
  } else {
    // Can't open memory file. Return error to our calling function and let it figure things out.
    return NOTGIOS_NOPROC;
  }
}

// Function is responsible for calculating %CPU usage of the monitored process.
// Currently uses the /proc pseudo-filesystem, which means that it's somewhat architecture independent,
// (certainly Linux only) but all of the reading I've done on this makes this sound like pretty much
// the only option.
int process_cpu_collect(uint16_t pid, task_report_t *data) {
  unsigned long start_pid_user, end_pid_user, start_pid_sys, end_pid_sys, start_pid_total, end_pid_total;
  unsigned long start_user, end_user, start_nice, end_nice, start_sys, end_sys, start_idle, end_idle, start_io, end_io;
  unsigned long start_global_total, end_global_total;
  int retvals[2];
  char path[NOTGIOS_MAX_PROC_LEN];

  // Need to get systemwide, and per process, CPU information from /proc filesystem.
  // I know that this isn't guaranteed to work on every system, but at least most Linuxes seem to agree on the
  // format for per process stat files, and that the first line of /proc/stat should be used for total CPU stats.
  sprintf(path, "/proc/%hu/stat", pid);
  FILE *pid_stats = fopen(path, "r");
  FILE *global_stats = fopen("/proc/stat", "r");

  // Perform some error checking here.
  // Process specific proc files only exist while their processes are running. If the process
  // crashed in between now and the time we checked it, the pid_stats fopen will fail.
  // If we're running on a distro that doesn't support any of these features, the fopens will
  // also presumably fail.
  // Don't know how likely any of this is, but we'll segfault if we don't check and it happens.
  if (!pid_stats && !global_stats) {
    return NOTGIOS_UNSUPP_DISTRO;
  } else if (!global_stats) {
    fclose(pid_stats);
    return NOTGIOS_UNSUPP_DISTRO;
  } else if (!pid_stats) {
    fclose(global_stats);
    return NOTGIOS_NOPROC;
  }

  // The call we've all been waiting for!
  // Get the processor times the first time.
  // The stars in the format strings represent that the value exists but that we're not interested in it.
  retvals[0] = fscanf(pid_stats, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %lu %lu", &start_pid_user, &start_pid_sys);
  retvals[1] = fscanf(global_stats, "%*s %lu %lu %lu %lu %lu", &start_user, &start_nice, &start_sys, &start_idle, &start_io);
  fclose(pid_stats);
  fclose(global_stats);

  // Make sure the scanning was successful, and calculate our first values.
  if (retvals[0] != 2 || retvals[1] != 5) return NOTGIOS_NOPROC;
  start_pid_total = start_pid_user + start_pid_sys;
  start_global_total = start_user + start_nice + start_sys + start_idle + start_io;

  // Sleep for a second and get updated statistics.
  sleep(1);

  // Reopen files for new values.
  pid_stats = fopen(path, "r");
  global_stats = fopen("/proc/stat", "r");

  // If we've made it this far, we know we're running a supported distro, but the process could
  // have crashed in the last second, so we need to check that again.
  if (!pid_stats) {
    fclose(global_stats);
    return NOTGIOS_NOPROC;
  }

  // Get the updated processor times.
  retvals[0] = fscanf(pid_stats, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %lu %lu", &end_pid_user, &end_pid_sys);
  retvals[1] = fscanf(global_stats, "%*s %lu %lu %lu %lu %lu", &end_user, &end_nice, &end_sys, &end_idle, &end_io);
  fclose(pid_stats);
  fclose(global_stats);

  // Same deal as before.
  if (retvals[0] != 2 || retvals[1] != 5) return NOTGIOS_NOPROC;
  end_pid_total = end_pid_user + end_pid_sys;
  end_global_total = end_user + end_nice + end_sys + end_idle + end_io;

  // Perform the calculation.
  data->percentage = (end_pid_total - start_pid_total) * 100 / (double) (end_global_total - start_global_total);
  data->time_taken = time(NULL);

  return NOTGIOS_SUCCESS;
}

// This function, will, someday attempt to discern a way to get per process IO statistics.
// For now it's unimplemented.
// TODO: Write this function.
int process_io_collect(uint16_t pid, task_report_t *data) {
  // I'll write this function someday when I have time.
  return NOTGIOS_UNSUPP_TASK;
}

long directory_memory_collect(char *path) {
  struct stat path_stat;
  stat(path, &path_stat);

  if (S_ISDIR(path_stat.st_mode)) {
    // We're working with a directory. Time to recursively calculate its size.
    DIR *directory = opendir(path);
    if (directory) {
      long size = 0;

      // Iterate across all entries in the directory.
      for (struct dirent *entry = readdir(directory); entry; entry = readdir(directory)) {
        // Skip the parent and current directory.
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
          // Recursively calculate size of entry.
          char *filename = malloc(sizeof(char) * (strlen(path) + strlen(entry->d_name) + 2));
          sprintf(filename, "%s/%s", path, entry->d_name);
          long retval = directory_memory_collect(filename);
          free(filename);

          // Increment size and keep going.
          if (retval >= 0) size += retval;
          else return retval;
        }
      }
      closedir(directory);

      // We're done, return the size of the directory.
      return size;
    } else {
      if (errno == EACCES) {
        return NOTGIOS_BAD_ACCESS;
      } else if (errno == EMFILE) {
        struct rlimit fd_limits;
        getrlimit(RLIMIT_NOFILE, &fd_limits);
        fd_limits.rlim_max *= 2;
        fd_limits.rlim_cur = fd_limits.rlim_max;
        int retval = setrlimit(RLIMIT_NOFILE, &fd_limits);
        if (!retval) return directory_memory_collect(path);
        else return NOTGIOS_NO_FILES;
      } else if (errno == ENFILE) {
        return NOTGIOS_NO_FILES;
      }
    }
  } else if (S_ISREG(path_stat.st_mode)) {
    // We're working with a file. Return the size.
    return (long) path_stat.st_size;
  }
}

int disk_memory_collect(uint16_t pid, task_report_t *data) {
  // TODO: Write this function.
}

// This function, will, someday attempt to discern a way to get per disk IO statistics.
// For now it's unimplemented.
// TODO: Write this function.
int disk_io_collect(uint16_t pid, task_report_t *data) {
  // I'll write this function someday when I have time.
  return NOTGIOS_UNSUPP_TASK;
}

int swap_collect(uint16_t pid, task_report_t *data) {
  // TODO: Write this function.
}

int load_collect(uint16_t pid, task_report_t *data) {
  // TODO: Write this function.
}

int total_memory_collect(task_report_t *data) {
  // Open the proc file for memory usage.
  FILE *mem_stats = fopen("/proc/meminfo", "r");
  long mem_total, mem_available;
  if (!mem_stats) return NOTGIOS_UNSUPP_DISTRO;

  // /proc/meminfo was apparently significantly changed with CentOS 7. Don't need to add
  // buffers/caches anymore, there's a field that shows actual memory free. Won't work on
  // older systems, but I'm not attempting to be compatible with anything before systemd
  // was added at the moment. Should fix this eventually.
  // FIXME: Incompatible with CentOS < 7.
  int retval = fscanf(mem_stats, "MemTotal: %ld kB\nMemFree: %*ld kB\nMemAvailable: %ld kB", &mem_total, &mem_available);
  fclose(mem_stats);
  if (retval != 2) return NOTGIOS_UNSUPP_DISTRO;

  data->percentage = mem_available / (double) mem_total;
  data->time_taken = time(NULL);
  return NOTGIOS_SUCCESS;
}

int total_cpu_collect(task_report_t *data) {
  unsigned long start_user, end_user, start_nice, end_nice, start_sys, end_sys, start_idle, end_idle, start_io, end_io;
  FILE *cpu_stats = fopen("/proc/stat", "r");
  if (!cpu_stats) return NOTGIOS_UNSUPP_DISTRO;

  // Get the initial values.
  int retval = fscanf(cpu_stats, "%*s %lu %lu %lu %lu %lu", &start_user, &start_nice, &start_sys, &start_idle, &start_io);
  if (retval != 5) {
    fclose(cpu_stats);
    return NOTGIOS_UNSUPP_DISTRO;
  }
  fclose(cpu_stats);

  // Sleep for a second.
  sleep(1);

  // Get the final values.
  cpu_stats = fopen("/proc/stat", "r");
  retval = fscanf(cpu_stats, "%*s %lu %lu %lu %lu %lu", &end_user, &end_nice, &end_sys, &end_idle, &end_io);
  if (retval != 5) return NOTGIOS_UNSUPP_DISTRO;

  // Perform the calculation.
  unsigned long start_idle_total = start_idle + start_io, end_idle_total = end_idle + end_io;
  unsigned long start_total = start_user + start_nice + start_sys + start_idle + start_io;
  unsigned long end_total = end_user + end_nice + end_sys + end_idle + end_io;
  unsigned long total_delta = end_total - start_total;
  data->percentage = (total_delta - (end_idle_total - start_idle_total)) / (double) total_delta;
  data->time_taken = time(NULL);
  
  return NOTGIOS_SUCCESS;
}

int total_io_collect(task_report_t *data) {
  // I'll write this function someday when I have time.
  return NOTGIOS_UNSUPP_TASK;
}

// Function checks whether or not it's possible to access memory statistics for our own process.
// If not, means that whatever distro we're running on doesn't support it.
int check_statm() {
  return !access("/proc/self/statm", F_OK);
}

// Function checks whether or not we can parse the CPU statistics for our own process.
// If not, means that whatever distro we're running on doesn't use a format we support.
int check_stat() {
  FILE *stats = fopen("/proc/self/stat", "r");
  FILE *global_stats = fopen("/proc/stat", "r");
  if (stats && global_stats) {
    int retvals[2], supported = 0;
    unsigned long pid_user, pid_sys, global_user, global_nice, global_sys, global_idle;

    // Parse that stuff.
    retvals[0] = fscanf(stats, "%*d %*s %*c %*d %*d %*d %*d %*d %*u %*lu %*lu %*lu %*lu %lu %lu", &pid_user, &pid_sys);
    retvals[1] = fscanf(global_stats, "%*s %lu %lu %lu %lu", &global_user, &global_nice, &global_sys, &global_idle);
    if (retvals[0] == 2 && retvals[1] == 4) supported = 1;
    
    fclose(global_stats);
    fclose(stats);
    return supported;
  } else if (!stats) {
    fclose(global_stats);
  } else if (!global_stats) {
    fclose(stats);
  }
  return 0;
}

void init_task_report(task_report_t *report, char *id, task_type_t type, metric_type_t metric) {
  if (report) {
    memset(report->id, 0, sizeof(char) * NOTGIOS_MAX_NUM_LEN);
    memset(report->message, 0, sizeof(char) * NOTGIOS_ERROR_BUFSIZE);

    strcpy(report->id, id);
    report->percentage = 0;
    report->value = 0;
    report->type = type;
    report->metric = metric;
  }
}
