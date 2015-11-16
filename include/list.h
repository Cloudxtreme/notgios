#ifndef LIST_H
#define LIST_H

/*----- System Includes -----*/

#include <pthread.h>

/*----- Numerical Constants -----*/

#define LIST_SUCCESS 0x0
#define LIST_FROZEN -0x01
#define LIST_NOMEM -0x02
#define LIST_INVAL -0x04
#define LIST_EMPTY -0x08

/*----- Type Declarations -----*/

// List node forward declaration.
typedef struct list_node list_node_t;

// Struct represents a threadsafe list.
typedef struct list {
  list_node_t *head, *tail;
  int count, dynamic, frozen, elem_len;
  pthread_mutex_t mutex;
  void (*destruct) (void *);
} list_t;

/*----- Function Declarations -----*/

list_t *create_list(int elem_len, void (*destruct) (void *));
int init_list(list_t *lst, int elem_len, void (*destruct) (void *));
int lpush(list_t *lst, void *data);
int rpop(list_t *lst, void *buf);
void destroy_list(list_t *lst);

#endif
