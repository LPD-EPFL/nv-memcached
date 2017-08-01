#ifndef _ACTIVE_SLABS_H_
#define _ACTIVE_SLABS_H_


#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stddef.h>
#include <libpmem.h>
#include <assert.h>
#include <libpmemobj.h>
#include <nv_memory.h>


#define AST_POOL_SIZE    (10 * 1024 * 1024) /* 1 MB */


#define LAYOUT_NAME "ast"

#define DEFAULT_SLAB_BUFFER_SIZE 32
#define CLEAN_THRESHOLD 16

#define MAX_NUM_SLABS 8192

typedef struct slab_descriptor_t {
    void* slab; 
    uint64_t lastAllocEpoch;
    uint64_t lastUnlinkEpoch;
} slab_descriptor_t;

typedef struct active_slab_table_t {
    size_t current_size;
    size_t last_in_use;
    uint8_t clear_all; // if flag set, I must clear the page buffer before accessing it again

    slab_descriptor_t slabs[MAX_NUM_SLABS]; // pages from which frees and allocs just happened
} active_slab_table_t;


POBJ_LAYOUT_BEGIN(ast);
POBJ_LAYOUT_ROOT(ast, active_slab_table_t);
POBJ_LAYOUT_END(ast);


//allocate a oage buffer already containing space for a predefined number of elements
active_slab_table_t* create_active_slab_table(uint32_t id);

//deallocate the bage buffer and entries
void destroy_active_slab_table(active_slab_table_t* to_delete);

//if a page is not present, add it to the buffer and persist the addition
void mark_slab(active_slab_table_t* slabs, void* ptr, void* slab, uint64_t currentTs, uint64_t collectTs, int isRemove);

//clear all the pages in the buffer
void clear_buffer(active_slab_table_t* slabs);

#endif
