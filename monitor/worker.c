/*----- System Includes -----*/

#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <syslog.h>
#include <pthread.h>

/*----- Local Includes -----*/

#include "worker.h"
#include "../include/hash.h"

/*----- Local Function Declaractions -----*/

void handle_process(metric_type_t metric, task_option_t *options, int id);
void handle_directory(task_option_t *options, int id);
void handle_disk(metric_type_t metric, task_option_t *options, int id);
void handle_swap(int id);
void handle_load(int id);

/*----- Evil but Necessary Globals -----*/

extern hash_t threads, controls;
extern pthread_rwlock_t connection_lock;
extern int connected, exiting;

/*----- Function Implementations -----*/

void run_task(task_type_t type, metric_type_t metric, task_option_t *options, int id) {
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
  }
}

void handle_process(metric_type_t metric, task_option_t *options, int id) {

}

void handle_directory(task_option_t *options, int id) {
  // TODO: Write this function.
}

void handle_disk(metric_type_t metric, task_option_t *options, int id) {
  // TODO: Write this function.
}

void handle_swap(int id) {
  // TODO: Write this function.
}

void handle_load(int id) {
  // TODO: Write this function.
}
