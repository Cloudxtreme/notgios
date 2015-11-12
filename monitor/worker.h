#ifndef WORKER_H
#define WORKER_H

/*----- Local Includes -----*/

#include "monitor.h"

/*----- Function Declarations -----*/

void run_task(task_type_t type, metric_type_t metric, task_option_t *options, char *id);

#endif
