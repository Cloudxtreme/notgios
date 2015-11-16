#ifndef WORKER_H
#define WORKER_H

/*----- Local Includes -----*/

#include "monitor.h"

/*----- Constant Declaractions -----*/

#define NOTGIOS_NOPROC -0x100
#define NOTGIOS_TASK_FATAL -0x200

/*----- Type Declaractions -----*/

typedef struct task_report {
  task_type_t type;
  metric_type_t metric;
  char id[NOTGIOS_MAX_NUM_LEN], message[NOTGIOS_ERROR_BUFSIZE];
  double percentage, value;
} task_report_t;

/*----- Function Declarations -----*/

int run_task(task_type_t type, metric_type_t metric, task_option_t *options, char *id);

#endif
