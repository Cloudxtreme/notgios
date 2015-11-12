#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <syslog.h>
#include <string.h>

#define DEFAULT_PIDFILE "/var/run/watchgios.pid"
#define NOTGIOS_MAX_ARGS 10

void launch_process();
void child_handler(int signal);
int check_pidfile(char *pidfile);
void user_error();

char *launch_path = NULL, *launch_args = NULL;

int main(int argc, char **argv) {
  int c;
  char *pidfile = NULL;

  opterr = 0;
  while ((c = getopt(argc, argv, "p:d:a:")) != -1) {
    switch (c) {
      case 'a':
        launch_args = optarg;
        break;
      case 'd':
        launch_path = optarg;
        break;
      case 'p':
        pidfile = optarg;
        break;
      default:
        user_error();
        return EXIT_FAILURE;
    }
  }
  if (!launch_path || !launch_args || !pidfile) {
    user_error();
    return EXIT_FAILURE;
  }

  int should_run = check_pidfile(pidfile);
  if (!should_run) return EXIT_FAILURE;
  launch_process();
  openlog("Notgios Watchdog", 0, 0);

  struct sigaction sa;
  sa.sa_handler = child_handler;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGCHLD, &sa, NULL)) {
    syslog(LOG_ERR, "Failed to install child signal handler. No way to recover, exiting...\n");
    return EXIT_FAILURE;
  }

  for (;;) pause();
}

void launch_process() {
  if (!fork()) {
    int counter = 1;
    char *args[NOTGIOS_MAX_ARGS];
    args[0] = launch_path;

    for (char *arg = strtok(launch_args, " "); arg && counter < NOTGIOS_MAX_ARGS - 1; arg = strtok(NULL, " ")) {
      args[counter++] = arg;
    }
    args[counter] = NULL;

    execv(launch_path, args);
  }
}

void child_handler(int signal) {
  int status = 0;
  pid_t pid = waitpid((pid_t) -1, &status, WUNTRACED | WCONTINUED);
  if (WIFEXITED(status) || WIFSIGNALED(status)) {
    syslog(LOG_INFO, "Notgios Monitor killed, restarting...\n");
    launch_process();
  } else if (WIFSTOPPED(status)) {
    syslog(LOG_INFO, "Notgios Monitor stopped, continuing...\n");
    kill(pid, SIGCONT);
  }
}

int check_pidfile(char *pidfile) {
  if (!access(pidfile, F_OK)) {
    short other_pid;
    FILE *file = fopen(pidfile, "r");
    int retval = fscanf(file, "%hd", &other_pid);
    fclose(file);

    if (retval && retval != EOF) {
      retval = kill(other_pid, 0);
      if (retval && errno == ESRCH) {
        file = fopen(pidfile, "w");
        fprintf(file, "%hd", (short) getpid());
        fclose(file);
        return 1;
      } else {
        if (errno == EPERM) user_error();
        return 0;
      }
    } else {
      return 1;
    }
  } else {
    FILE *file;
    if (errno == ENOENT) file = fopen(pidfile, "w+");
    else return check_pidfile(DEFAULT_PIDFILE);
    fprintf(file, "%hd", (short) getpid());
    fclose(file);
    return 1;
  }
}

void user_error() {
  fprintf(stderr, "This utility is used internally by the Notgios host monitoring framework, and is not meant to be launched manually.\n");
  exit(EINVAL);
}
