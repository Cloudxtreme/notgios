#ifndef HASH_H
#define HASH_H

/*----- System Includes -----*/

#include <pthread.h>

/*----- Numerical Constants -----*/

#define HASH_START_SIZE 10
#define HASH_SUCCESS 0x0
#define HASH_FROZEN -0x01
#define HASH_NOMEM -0x02
#define HASH_INVAL -0x04
#define HASH_EXISTS -0x08
#define HASH_NOTFOUND -0x10

/*----- Struct Declarations -----*/

typedef struct hash_node hash_node_t;

// Struct represents a basic hashtable.
typedef struct hash {
  hash_node_t **data;
  void (*destruct) (void *);
  int count, size, dynamic, frozen;
  pthread_rwlock_t lock;
} hash_t;

/*----- Hash Functions -----*/

hash_t *create_hash(void (*destruct) (void *));
int init_hash(hash_t *table, void (*destruct) (void *));
int hash_put(hash_t *table, char *key, void *data);
void *hash_get(hash_t *table, char *key);
int hash_drop(hash_t *table, char *key);
char **hash_keys(hash_t *table);
void hash_freeze(hash_t *table);
void destroy_hash(hash_t *table);

#endif
