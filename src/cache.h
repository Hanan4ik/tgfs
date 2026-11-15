#ifndef TGFS_CACHE_H
#define TGFS_CACHE_H
#include "internal.h"
typedef struct tgfs_cache tgfs_cache;
int cache_open(const char *directory,size_t limit,tgfs_cache **out,tgfs_error *e);
void cache_close(tgfs_cache *cache);
int cache_hash(const void *bytes,size_t length,char hash[65],tgfs_error *e);
int cache_json_key(const cJSON *value,char key[65],tgfs_error *e);
/* 1 hit (malloc buffer), 0 miss/corruption evicted, -1 storage error. */
int cache_blob_get(tgfs_cache *cache,const char *key,size_t limit,unsigned char **bytes,size_t *length,tgfs_error *e);
int cache_blob_put(tgfs_cache *cache,const char *key,const unsigned char *bytes,size_t length,tgfs_error *e);
int cache_blob_drop(tgfs_cache *cache,const char *key,tgfs_error *e);
int cache_index_get(tgfs_cache *cache,const char *key,cJSON **snapshot,tgfs_error *e);
int cache_index_put(tgfs_cache *cache,const char *key,const cJSON *snapshot,const cJSON *journal,tgfs_error *e);
#endif
