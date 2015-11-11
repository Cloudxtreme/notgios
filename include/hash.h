#ifndef HASH_H
#define HASH_H

/*----- Numerical Constants -----*/

#define HASH_START_SIZE 10

/*----- Struct Declarations -----*/

typedef struct hash_node hash_node_t;

// Struct represents a basic hashtable.
typedef struct hash {
  hash_node_t **data;
  void (*destruct) (void *);
  int count;
  int size;
} hash_t;

/*----- Hash Functions -----*/

hash_t *create_hash(void (*destruct) (void *));
int init_hash(hash_t *table, void (*destruct) (void *));
int hash_put(hash_t *table, char *key, void *data);
void *hash_get(hash_t *table, char *key);
char **hash_get_keys(hash_t *table);
int hash_drop(hash_t *table, char *key);
void destroy_hash(hash_t *table);

#endif
