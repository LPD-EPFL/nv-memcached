/* slabs memory allocation */
#ifndef SLABS_H
#define SLABS_H

// TODO: in the future, make slabs pool size variable, according to maximum size of cache
#define SLABS_POOL_SIZE    (10* 1024 * 1024 * 1024)

/** Init the subsystem. 1st argument is the limit on no. of bytes to allocate,
    0 if no limit. 2nd argument is the growth factor; each slab will use a chunk
    size equal to the previous slab's chunk size times this factor.
    3rd argument specifies if the slab allocator should allocate all memory
    up front (if true), or allocate memory in chunks as it is needed (if false)
*/
void slabs_init(const size_t limit, const double factor, const bool prealloc);


/**
 * Given object size, return id to use when allocating/freeing memory for object
 * 0 means error: can't store such a large object
 */

unsigned int slabs_clsid(const size_t size);

/** Allocate object of given length. 0 on error */ /*@null@*/
void *slabs_alloc(const size_t size, unsigned int id, unsigned int *total_chunks);

/** Free previously allocated object */
void slabs_free(void *ptr, size_t size, unsigned int id);

/** Adjust the stats for memory requested */
void slabs_adjust_mem_requested(unsigned int id, size_t old, size_t ntotal);

/** Return a datum for stats in binary protocol */
bool get_stats(const char *stat_type, int nkey, ADD_STAT add_stats, void *c);

/** Fill buffer with stats */ /*@null@*/
void slabs_stats(ADD_STAT add_stats, void *c);

/* Hints as to freespace in slab class */
unsigned int slabs_available_chunks(unsigned int id, bool *mem_flag, unsigned int *total_chunks);

#ifdef NVM
void clock_update(item* it);
item* clock_get_victim(unsigned int id);
#endif

int start_slab_maintenance_thread(void);
void stop_slab_maintenance_thread(void);

enum reassign_result_type {
    REASSIGN_OK=0, REASSIGN_RUNNING, REASSIGN_BADCLASS, REASSIGN_NOSPARE,
    REASSIGN_SRC_DST_SAME
};

enum reassign_result_type slabs_reassign(int src, int dst);

void slabs_rebalancer_pause(void);
void slabs_rebalancer_resume(void);

struct _slabclass;
typedef _slabclass slabclass_t;

typedef void* void_p;

POBJ_LAYOUT_BEGIN(slabs);
POBJ_LAYOUT_ROOT(slabs, struct slab_root);
POBJ_LAYOUT_TOID(slabs, slabclass_t);
POBJ_LAYOUT_TOID(slabs, char);
POBJ_LAYOUT_TOID(slabs, void_p);
POBJ_LAYOUT_END(slabs);


#endif
