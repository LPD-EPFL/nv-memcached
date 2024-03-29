/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 * Slabs memory allocation, based on powers-of-N. Slabs are up to 1MB in size
 * and are divided into chunks. The chunk sizes start off at the size of the
 * "item" structure plus space for a small key and value. They increase by
 * a multiplier factor from there, up to half the maximum slab size. The last
 * slab size is always 1MB, since that's the maximum item size allowed by the
 * memcached protocol.
 */
#include "memcached.h"
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/resource.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

/* powers-of-N allocation structures */

struct _slabclass {
    unsigned int size;      /* sizes of items */
    unsigned int perslab;   /* how many items per slab */

    void *slots;           /* list of item ptrs */
    unsigned int sl_curr;   /* total free items in list */

    unsigned int slabs;     /* how many slabs were allocated for this class */

#ifdef NVM
    TOID(void_p) slab_list;
#else


    void **slab_list;       /* array of slab pointers */
#endif
    unsigned int list_size; /* size of prev array */

    unsigned int killing;  /* index+1 of dying slab, or zero if none */
    size_t requested; /* The number of requested bytes */

#ifdef NVM
    TOID(char) bitmap;             /* bit per slot for clock alg. */
    unsigned int clock_hand;  /* current slot for clock alg. */
#endif
};

struct slab_root {
    slabclass_t slabclass[MAX_NUMBER_OF_SLAB_CLASSES];
    size_t mem_limit = 0;
    size_t mem_malloced = 0;
    /* If the memory limit has been hit once. Used as a hint to decide when to
     * early-wake the LRU maintenance thread */
    bool mem_limit_reached = false;
    int power_largest;

    void *mem_base = NULL;
    void *mem_current = NULL;
    size_t mem_avail = 0;
};

static slab_root* root;
static PMEMobjpool *pop = NULL;

/**
 * Access to the slab allocator is protected by this lock
 */
static pthread_mutex_t slabs_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t slabs_rebalance_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Forward Declarations
 */
static int do_slabs_newslab(const unsigned int id);
static void *memory_allocate(size_t size);
static void do_slabs_free(void *ptr, const size_t size, unsigned int id);

/* Preallocate as many slab pages as possible (called from slabs_init)
   on start-up, so users don't get confused out-of-memory errors when
   they do have free (in-slab) space, but no space to make new slabs.
   if maxslabs is 18 (POWER_LARGEST - POWER_SMALLEST + 1), then all
   slab types can be made.  if max memory is less than 18 MB, only the
   smaller ones will be made.  */
static void slabs_preallocate (const unsigned int maxslabs);

/*
 * Figures out which slab class (chunk size) is required to store an item of
 * a given size.
 *
 * Given object size, return id to use when allocating/freeing memory for object
 * 0 means error: can't store such a large object
 */

unsigned int slabs_clsid(const size_t size) {
    int res = POWER_SMALLEST;

    if (size == 0)
        return 0;
    while (size > root->slabclass[res].size)
        if (res++ == root->power_largest)     /* won't fit in the biggest slab */
            return 0;
    return res;
}

/**
 * Determines the chunk sizes and initializes the slab class descriptors
 * accordingly.
 */
void slabs_init(const size_t limit, const double factor, const bool prealloc) {

    int i = POWER_SMALLEST - 1;
    unsigned int size = sizeof(item) + settings.chunk_size;

    // Start setting up pmemobj pool
    char path[32];
    sprintf(path, "/tmp/slabs");

    remove(path);

    if (access(path, F_OK) != 0) {
        if ((pop = pmemobj_create(path, POBJ_LAYOUT_NAME(slabs),
            SLABS_POOL_SIZE, S_IWUSR | S_IRUSR)) == NULL) {
            printf("failed to create pool1 wiht name %s\n", path);
            exit(1);
        }
    } else {
      if ((pop = pmemobj_open(path, POBJ_LAYOUT_NAME(slabs))) == NULL) {
          printf("failed to open pool with name %s\n", path);
          exit(1);
      }
    }

    TOID(struct slab_root) _root = POBJ_ROOT(pop, struct slab_root);
    root = D_RW(_root);
    // Done setting up pmemobj pool

    TX_BEGIN(pop) {
        TX_ADD_DIRECT(root);
        root->mem_limit = limit;


        if (prealloc) {
            /* Allocate everything in a big chunk with malloc */
            root->mem_base = (void*)D_RW(TX_ALLOC(char, root->mem_limit));
            if (root->mem_base != NULL) {
                root->mem_current = root->mem_base;
                root->mem_avail = root->mem_limit;
            } else {
                fprintf(stderr, "Warning: Failed to allocate requested memory in"
                        " one large chunk.\nWill allocate in smaller chunks\n");
            }
        }

        TX_MEMSET(root->slabclass, 0, sizeof(root->slabclass));

        while (++i < MAX_NUMBER_OF_SLAB_CLASSES-1 && size <= settings.item_size_max / factor) {
            /* Make sure items are always n-byte aligned */
            if (size % CHUNK_ALIGN_BYTES)
                size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);

            root->slabclass[i].size = size;
            root->slabclass[i].perslab = settings.item_size_max / root->slabclass[i].size;
            size *= factor;
            if (settings.verbose > 1) {
                fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                        i, root->slabclass[i].size, root->slabclass[i].perslab);
            }
        }

        root->power_largest = i;
        root->slabclass[root->power_largest].size = settings.item_size_max;
        root->slabclass[root->power_largest].perslab = 1;
        if (settings.verbose > 1) {
            fprintf(stderr, "slab class %3d: chunk size %9u perslab %7u\n",
                    i, root->slabclass[i].size, root->slabclass[i].perslab);
        }

        /* for the test suite:  faking of how much we've already malloc'd */
        {
            char *t_initial_malloc = getenv("T_MEMD_INITIAL_MALLOC");
            if (t_initial_malloc) {
                root->mem_malloced = (size_t)atol(t_initial_malloc);
            }

        }

        if (prealloc) {
            slabs_preallocate(root->power_largest);
        }
    } TX_END

}

static void slabs_preallocate (const unsigned int maxslabs) {
    TX_BEGIN(pop) {
        int i;
        unsigned int prealloc = 0;

        /* pre-allocate a 1MB slab in every size class so people don't get
           confused by non-intuitive "SERVER_ERROR out of memory"
           messages.  this is the most common question on the mailing
           list.  if you really don't want this, you can rebuild without
           these three lines.  */

        for (i = POWER_SMALLEST; i < MAX_NUMBER_OF_SLAB_CLASSES; i++) {
            if (++prealloc > maxslabs)
                return;
            if (do_slabs_newslab(i) == 0) {
                fprintf(stderr, "Error while preallocating slab memory!\n"
                    "If using -L or other prealloc options, max memory must be "
                    "at least %d megabytes.\n", root->power_largest);
                exit(1);
            }
        }
    } TX_END
}

static int grow_slab_list (const unsigned int id) {
    TX_BEGIN(pop) {
        slabclass_t *p = &root->slabclass[id];
        if (p->slabs == p->list_size) {
            size_t new_size =  (p->list_size != 0) ? p->list_size * 2 : 16;
            TOID(void_p) new_list = TX_REALLOC(p->slab_list, new_size * sizeof(void *));
            if (TOID_IS_NULL(new_list)) return 0;
            p->list_size = new_size;
            p->slab_list = new_list;
        }
        return 1;
        
    } TX_END
}

static void assign_slab_ids(const unsigned int id, const void* slab) {
    TX_BEGIN(pop) {
        slabclass_t *p = &root->slabclass[id];
        char* ptr = (char*)slab;
        int x;
        for (x = 0; x < p->perslab; x++) {
            item* it = (item*) ptr;
            it->slab = (void*)slab;
            ptr += p->size;
        }
    } TX_END
}

static void split_slab_page_into_freelist(char *ptr, const unsigned int id) {
    TX_BEGIN(pop) {
        slabclass_t *p = &root->slabclass[id];
        int x;
        for (x = 0; x < p->perslab; x++) {
            do_slabs_free(ptr, 0, id);
            ptr += p->size;
        }
    } TX_END
}

#ifdef NVM
// Initial contents of bitmap don't matter, since we set bit when item is used
static int clock_grow_bitmap(const unsigned int id) {
    TX_BEGIN(pop) {
        slabclass_t* p = &root->slabclass[id];
        unsigned int total_slots = (p->slabs + 1) * p->perslab; //nakon ove fje se slabs inkr
        unsigned int bitmap_size = (total_slots + 7) / 8;

        if (TOID_IS_NULL(p->bitmap)) {
            p->bitmap = TX_ALLOC(char, bitmap_size);
            if (TOID_IS_NULL(p->bitmap))
                return 0;
        } else {
            TOID(char) new_bitmap = TX_REALLOC(p->bitmap, bitmap_size);
            if (TOID_IS_NULL(new_bitmap))
                return 0;
            p->bitmap = new_bitmap;
        }
        return 1;
    } TX_END
}

static void set_item_indices(char *ptr, const unsigned int id) {
    TX_BEGIN(pop){
        slabclass_t* p = &root->slabclass[id];
        unsigned int current_items = p->slabs * p->perslab;

        int i;
        for (i = 0; i < p->perslab; i++) {
            item* it = (item*)ptr;
            it->slabs_index = current_items + i;
            ptr += p->size;
        }
    } TX_END
}
#endif

static int do_slabs_newslab(const unsigned int id) {
    TX_BEGIN(pop) {
        slabclass_t *p = &root->slabclass[id];
        int len = settings.slab_reassign ? settings.item_size_max
            : p->size * p->perslab;
        char *ptr;

        if ((root->mem_limit && root->mem_malloced + len > root->mem_limit && p->slabs > 0)) {
            root->mem_limit_reached = true;
            MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
            return 0;
        }

        if ((grow_slab_list(id) == 0) ||
#ifdef NVM
            (clock_grow_bitmap(id) == 0) ||
#endif
            ((ptr = (char*)memory_allocate((size_t)len)) == 0)) {

            MEMCACHED_SLABS_SLABCLASS_ALLOCATE_FAILED(id);
            return 0;
        }

        memset(ptr, 0, (size_t)len);
        split_slab_page_into_freelist(ptr, id);
        assign_slab_ids(id, ptr);


#ifdef NVM
        set_item_indices(ptr, id);
#endif

        D_RW(p->slab_list)[p->slabs++] = ptr;
        root->mem_malloced += len;
        MEMCACHED_SLABS_SLABCLASS_ALLOCATE(id);

        return 1;
    } TX_END
}

/*@null@*/
static void *do_slabs_alloc(const size_t size, unsigned int id, unsigned int *total_chunks) {
    slabclass_t *p;
    void *ret = NULL;
    item *it = NULL;

    if (id < POWER_SMALLEST || id > root->power_largest) {
        MEMCACHED_SLABS_ALLOCATE_FAILED(size, 0);
        return NULL;
    }
    p = &root->slabclass[id];
    assert(p->sl_curr == 0 || ((item *)p->slots)->slabs_clsid == 0);

    *total_chunks = p->slabs * p->perslab;
    /* fail unless we have space at the end of a recently allocated page,
       we have something on our freelist, or we could allocate a new page */
    if (! (p->sl_curr != 0 || do_slabs_newslab(id) != 0)) {
        /* We don't have more memory available */
        ret = NULL;
    } else if (p->sl_curr != 0) {
        /* return off our freelist */
        TX_BEGIN(pop) {
            it = (item *)p->slots;
            p->slots = it->next;
            if (it->next) it->next->prev = 0;

            /* Kill flag and initialize refcount here for lock safety in slab
             * mover's freeness detection. */
            it->it_flags &= ~ITEM_SLABBED;
#ifndef NVM
            it->refcount = 1;
#endif

            p->sl_curr--;
        } TX_ONCOMMIT {
            active_slab_table_t* my_slab_table = getMySlabTable();
            uint64_t my_current_timestamp = getMyTimestamp();
            uint64_t my_last_collect = getMyLastCollect();
            mark_slab(my_slab_table, it, it->slab, it->slabs_clsid, my_current_timestamp, my_last_collect, 0);
        } TX_FINALLY {
            ret = (void *)it;
        } TX_END
    }

    if (ret) {
        p->requested += size;
        MEMCACHED_SLABS_ALLOCATE(size, id, p->size, ret);
    } else {
        MEMCACHED_SLABS_ALLOCATE_FAILED(size, id);
    }

    return ret;
}

static void do_slabs_free(void *ptr, const size_t size, unsigned int id) {
    slabclass_t *p;
    item *it;

    assert(id >= POWER_SMALLEST && id <= root->power_largest);
    if (id < POWER_SMALLEST || id > root->power_largest)
        return;

    MEMCACHED_SLABS_FREE(size, id, ptr);
    p = &root->slabclass[id];

    it = (item *)ptr;
    it->slabs_clsid = 0;

    it->prev = 0;
    it->next = (item*)p->slots;
    if (it->next) it->next->prev = it;
    p->slots = it;
    it->it_flags |= ITEM_SLABBED;

    p->sl_curr++;
    p->requested -= size;
    return;
}

void slabs_recover(active_slab_table_t** slab_tables, ht_intset_t* ht, int num_threads) {
    slabclass_t* p;
    size_t i,j,k;
    size_t num_slabs;
    slab_descriptor_t* crt;
    char* current_address;

    for (i = 0; i < num_threads; i++) {
        num_slabs = slab_tables[i]->last_in_use;
        crt = slab_tables[i]->slabs;
        for (j = 0; j < num_slabs; j++) {
            current_address = (char*)crt[j].slab;
            if (current_address != NULL) {
                p = &root->slabclass[crt[j].slabs_clsid];            
                for (k = 0; k < p->perslab; k++) {
                    item* it = (item*)current_address;
                    if ((it->it_flags & ITEM_SLABBED) == 0) {
                        if (!item_is_reachable(ht, (void*)it)) {

                            // remove it from whatever list it is in
                            if (it->prev) {
                                it->prev->next = it->next;
                            }
                            if (it->next) {
                                it->next->prev = it->prev;
                            }
                            // add item to slots
                            it->prev = 0;
                            it->next = (item*)p->slots;
                            if (it->next) it->next->prev = it;
                            p->slots = it;
                            // mark item slabbed
                            it->it_flags |= ITEM_SLABBED;
                        }
                    }
                    current_address += p->size;
                }
            }
        }
    }
}


static int nz_strcmp(int nzlength, const char *nz, const char *z) {
    int zlength=strlen(z);
    return (zlength == nzlength) && (strncmp(nz, z, zlength) == 0) ? 0 : -1;
}

bool get_stats(const char *stat_type, int nkey, ADD_STAT add_stats, void *c) {
    bool ret = true;

    if (add_stats != NULL) {
        if (!stat_type) {
            /* prepare general statistics for the engine */
            STATS_LOCK();
            APPEND_STAT("bytes", "%llu", (unsigned long long)stats.curr_bytes);
            APPEND_STAT("curr_items", "%u", stats.curr_items);
            APPEND_STAT("total_items", "%u", stats.total_items);
            STATS_UNLOCK();
            item_stats_totals(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "items") == 0) {
            item_stats(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "slabs") == 0) {
            slabs_stats(add_stats, c);
        } else if (nz_strcmp(nkey, stat_type, "sizes") == 0) {
            item_stats_sizes(add_stats, c);
        } else {
            ret = false;
        }
    } else {
        ret = false;
    }

    return ret;
}

/*@null@*/
static void do_slabs_stats(ADD_STAT add_stats, void *c) {
    int i, total;
    /* Get the per-thread stats which contain some interesting aggregates */
    struct thread_stats thread_stats;
    threadlocal_stats_aggregate(&thread_stats);

    total = 0;
    for(i = POWER_SMALLEST; i <= root->power_largest; i++) {
        slabclass_t *p = &root->slabclass[i];
        if (p->slabs != 0) {
            uint32_t perslab, slabs;
            slabs = p->slabs;
            perslab = p->perslab;

            char key_str[STAT_KEY_LEN];
            char val_str[STAT_VAL_LEN];
            int klen = 0, vlen = 0;

            APPEND_NUM_STAT(i, "chunk_size", "%u", p->size);
            APPEND_NUM_STAT(i, "chunks_per_page", "%u", perslab);
            APPEND_NUM_STAT(i, "total_pages", "%u", slabs);
            APPEND_NUM_STAT(i, "total_chunks", "%u", slabs * perslab);
            APPEND_NUM_STAT(i, "used_chunks", "%u",
                            slabs*perslab - p->sl_curr);
            APPEND_NUM_STAT(i, "free_chunks", "%u", p->sl_curr);
            /* Stat is dead, but displaying zero instead of removing it. */
            APPEND_NUM_STAT(i, "free_chunks_end", "%u", 0);
            APPEND_NUM_STAT(i, "mem_requested", "%llu",
                            (unsigned long long)p->requested);
            APPEND_NUM_STAT(i, "get_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].get_hits);
            APPEND_NUM_STAT(i, "cmd_set", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].set_cmds);
            APPEND_NUM_STAT(i, "delete_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].delete_hits);
            APPEND_NUM_STAT(i, "incr_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].incr_hits);
            APPEND_NUM_STAT(i, "decr_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].decr_hits);
            APPEND_NUM_STAT(i, "cas_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].cas_hits);
            APPEND_NUM_STAT(i, "cas_badval", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].cas_badval);
            APPEND_NUM_STAT(i, "touch_hits", "%llu",
                    (unsigned long long)thread_stats.slab_stats[i].touch_hits);
            total++;
        }
    }

    /* add overall slab stats and append terminator */

    APPEND_STAT("active_slabs", "%d", total);
    APPEND_STAT("total_malloced", "%llu", (unsigned long long)root->mem_malloced);
    add_stats(NULL, 0, NULL, 0, c);
}

static void *memory_allocate(size_t size) {
    void *ret;

    TX_BEGIN(pop) {
        if (root->mem_base == NULL) {
            /* We are not using a preallocated large memory chunk */
            ret = (void*)D_RW(TX_ALLOC(char, size));
        } else {
            ret = root->mem_current;

            if (size > root->mem_avail) {
                return NULL;
            }

            /* mem_current pointer _must_ be aligned!!! */
            if (size % CHUNK_ALIGN_BYTES) {
                size += CHUNK_ALIGN_BYTES - (size % CHUNK_ALIGN_BYTES);
            }

            root->mem_current = ((char*)root->mem_current) + size;
            if (size < root->mem_avail) {
                root->mem_avail -= size;
            } else {
                root->mem_avail = 0;
            }
        }

    } TX_END 
    return ret;
}

void *slabs_alloc(size_t size, unsigned int id, unsigned int *total_chunks) {
    void *ret;

    pthread_mutex_lock(&slabs_lock);
    ret = do_slabs_alloc(size, id, total_chunks);
    pthread_mutex_unlock(&slabs_lock);
    return ret;
}

void slabs_free(void *ptr, size_t size, unsigned int id) {
    pthread_mutex_lock(&slabs_lock);
    do_slabs_free(ptr, size, id);
    pthread_mutex_unlock(&slabs_lock);
}

void slabs_stats(ADD_STAT add_stats, void *c) {
    pthread_mutex_lock(&slabs_lock);
    do_slabs_stats(add_stats, c);
    pthread_mutex_unlock(&slabs_lock);
}

void slabs_adjust_mem_requested(unsigned int id, size_t old, size_t ntotal)
{
    pthread_mutex_lock(&slabs_lock);
    slabclass_t *p;
    if (id < POWER_SMALLEST || id > root->power_largest) {
        fprintf(stderr, "Internal error! Invalid slab class\n");
        abort();
    }

    p = &root->slabclass[id];
    p->requested = p->requested - old + ntotal;
    pthread_mutex_unlock(&slabs_lock);
}

unsigned int slabs_available_chunks(const unsigned int id, bool *mem_flag,
        unsigned int *total_chunks) {
    unsigned int ret;
    slabclass_t *p;

    pthread_mutex_lock(&slabs_lock);
    p = &root->slabclass[id];
    ret = p->sl_curr;
    if (mem_flag != NULL)
        *mem_flag = root->mem_limit_reached;
    if (total_chunks != NULL)
        *total_chunks = p->slabs * p->perslab;
    pthread_mutex_unlock(&slabs_lock);
    return ret;
}

#ifdef NVM
static inline int clock_get_bit(TOID(char) bitmap, unsigned int index) {
    unsigned int byte_index = index >> 3;  // index / 8
    char mask = 1 << (index & 7);          // index % 8
    return D_RW(bitmap)[byte_index] & mask;
}

static inline void clock_set_bit(TOID(char) bitmap, unsigned int index) {
    unsigned int byte_index = index >> 3;
    char mask = 1 << (index & 7);
    D_RW(bitmap)[byte_index] |= mask;
}

static inline void clock_reset_bit(TOID(char) bitmap, unsigned int index) {
    unsigned int byte_index = index >> 3;
    char mask = ~(1 << (index & 7));
    D_RW(bitmap)[byte_index] &= mask;
}

static void* slabs_get_slot_at_index(unsigned int index, unsigned int id) {
    slabclass_t* p = &root->slabclass[id];

    //assert(index < p->slabs * p->perslab);

    unsigned int slab_index = index / p->perslab;
    unsigned int slot_index = index % p->perslab;

    char* ret = (char*)D_RW(p->slab_list)[slab_index] + slot_index*p->size;

    return (void*)ret;
}

static short firstzero[256] = {
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 6,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 7,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 6,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 5,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 4,
    0, 1, 0, 2, 0, 1, 0, 3, 0, 1, 0, 2, 0, 1, 0, 8
};

void clock_update(item* it) {
    unsigned int id = ITEM_clsid(it);
    assert(id >= POWER_SMALLEST && id <= root->power_largest);
    if (id < POWER_SMALLEST || id > root->power_largest)
        return;

    slabclass_t* p = &root->slabclass[id];

    clock_set_bit(p->bitmap, it->slabs_index);
}

item* clock_get_victim(unsigned int id) {
    slabclass_t* p = &root->slabclass[id];

    pthread_mutex_lock(&slabs_lock);

    //unsigned 
    int total_slots = p->slabs * p->perslab;
    assert(p->clock_hand < total_slots);
    

    p->clock_hand++;

    while (1) {
        int victim_found = 0;

        // If there is less than a byte left until the end of bitmap
        //unsigned 
        int slots_left = total_slots - p->clock_hand; //- 1;

        if (slots_left < 8) {
            while (slots_left>0) {

                if (clock_get_bit(p->bitmap, p->clock_hand)) {
                    clock_reset_bit(p->bitmap, p->clock_hand);
                } else {
                    victim_found = 1;
                    break;
                }
                p->clock_hand++;
                slots_left--;
            }
            if (victim_found)
                break;
            p->clock_hand = 0;
            slots_left = total_slots;
        }

        // Search until the end of current byte (if clock_hand % 8 != 0)
        while ((p->clock_hand & 0x7) != 0) {

            if (clock_get_bit(p->bitmap, p->clock_hand)) {
                clock_reset_bit(p->bitmap, p->clock_hand);
            } else {
                victim_found = 1;
                break;
            }
            p->clock_hand++;
        }
        if (victim_found)
            break;


        int found_in_64 = 0;
        // Search in 64bit increments until 0 is found
        slots_left = total_slots - p->clock_hand;// - 1;
        while (slots_left >= 64) {
            uint64_t* val64 = (uint64_t*)(D_RW(p->bitmap) + (p->clock_hand >> 3));
            if (*val64 == (uint64_t)-1) {
                *val64 = 0;
                p->clock_hand += 64;
                slots_left -= 64;
            } else {
                found_in_64=1;
                break;
            }
        }

        if (!found_in_64)
        {
            p->clock_hand = (p->clock_hand >> 3) <<3;
            slots_left = total_slots - p->clock_hand;
        }

        // Search in byte increments until 0 is found
        while (slots_left >= 8) {
            uint8_t* val8 = (uint8_t*)(D_RW(p->bitmap) + (p->clock_hand >> 3));
            if (*val8 == (uint8_t)-1) {
                *val8 = 0;
                p->clock_hand += 8;
                slots_left -= 8;
            } else {
                p->clock_hand += firstzero[*val8];
                victim_found = 1;
                break;
            }
        }
        if (victim_found)
            break;

        if (slots_left<=0)
        {   
            p->clock_hand = 0;
            slots_left = total_slots;
        }

    } //while (1)

    item* it = (item*)slabs_get_slot_at_index(p->clock_hand, id);

    pthread_mutex_unlock(&slabs_lock);

    return it;
}
#endif

static pthread_cond_t slab_rebalance_cond = PTHREAD_COND_INITIALIZER;
static volatile int do_run_slab_thread = 1;
static volatile int do_run_slab_rebalance_thread = 1;

#define DEFAULT_SLAB_BULK_CHECK 1
int slab_bulk_check = DEFAULT_SLAB_BULK_CHECK;

static int slab_rebalance_start(void) {
    slabclass_t *s_cls;
    int no_go = 0;

    pthread_mutex_lock(&slabs_lock);

    if (slab_rebal.s_clsid < POWER_SMALLEST ||
        slab_rebal.s_clsid > root->power_largest  ||
        slab_rebal.d_clsid < POWER_SMALLEST ||
        slab_rebal.d_clsid > root->power_largest  ||
        slab_rebal.s_clsid == slab_rebal.d_clsid)
        no_go = -2;

    s_cls = &root->slabclass[slab_rebal.s_clsid];

    if (!grow_slab_list(slab_rebal.d_clsid)) {
        no_go = -1;
    }

    if (s_cls->slabs < 2)
        no_go = -3;

    if (no_go != 0) {
        pthread_mutex_unlock(&slabs_lock);
        return no_go; /* Should use a wrapper function... */
    }

    s_cls->killing = 1;

    slab_rebal.slab_start = D_RW(s_cls->slab_list)[s_cls->killing - 1];
    slab_rebal.slab_end   = (char *)slab_rebal.slab_start +
        (s_cls->size * s_cls->perslab);
    slab_rebal.slab_pos   = slab_rebal.slab_start;
    slab_rebal.done       = 0;

    /* Also tells do_item_get to search for items in this slab */
    slab_rebalance_signal = 2;

    if (settings.verbose > 1) {
        fprintf(stderr, "Started a slab rebalance\n");
    }

    pthread_mutex_unlock(&slabs_lock);

    STATS_LOCK();
    stats.slab_reassign_running = true;
    STATS_UNLOCK();

    return 0;
}

enum move_status {
    MOVE_PASS=0, MOVE_FROM_SLAB, MOVE_FROM_LRU, MOVE_BUSY, MOVE_LOCKED
};

/* refcount == 0 is safe since nobody can incr while item_lock is held.
 * refcount != 0 is impossible since flags/etc can be modified in other
 * threads. instead, note we found a busy one and bail. logic in do_item_get
 * will prevent busy items from continuing to be busy
 * NOTE: This is checking it_flags outside of an item lock. I believe this
 * works since it_flags is 8 bits, and we're only ever comparing a single bit
 * regardless. ITEM_SLABBED bit will always be correct since we're holding the
 * lock which modifies that bit. ITEM_LINKED won't exist if we're between an
 * item having ITEM_SLABBED removed, and the key hasn't been added to the item
 * yet. The memory barrier from the slabs lock should order the key write and the
 * flags to the item?
 * If ITEM_LINKED did exist and was just removed, but we still see it, that's
 * still safe since it will have a valid key, which we then lock, and then
 * recheck everything.
 * This may not be safe on all platforms; If not, slabs_alloc() will need to
 * seed the item key while holding slabs_lock.
 */
static int slab_rebalance_move(void) {
    slabclass_t *s_cls;
    int x; 
    int was_busy = 0;
    int refcount = 0;
    uint32_t hv;
    void *hold_lock;
    enum move_status status = MOVE_PASS;

    pthread_mutex_lock(&slabs_lock);

    s_cls = &root->slabclass[slab_rebal.s_clsid];

    for (x = 0; x < slab_bulk_check; x++) {
        hv = 0;
        hold_lock = NULL;
        item *it = (item*)slab_rebal.slab_pos;
        status = MOVE_PASS;
        if (it->slabs_clsid != 255) {
            /* ITEM_SLABBED can only be added/removed under the slabs_lock */
            if (it->it_flags & ITEM_SLABBED) {
                /* remove from slab freelist */
                if (s_cls->slots == it) {
                    s_cls->slots = it->next;
                }
                if (it->next) it->next->prev = it->prev;
                if (it->prev) it->prev->next = it->next;
                s_cls->sl_curr--;
                status = MOVE_FROM_SLAB;
            } else if ((it->it_flags & ITEM_LINKED) != 0) {
                /* If it doesn't have ITEM_SLABBED, the item could be in any
                 * state on its way to being freed or written to. If no
                 * ITEM_SLABBED, but it's had ITEM_LINKED, it must be active
                 * and have the key written to it already.
                 */
                hv = hash(ITEM_key(it), it->nkey);
                if ((hold_lock = item_trylock(hv)) == NULL) {
                    status = MOVE_LOCKED;
                } else {
                    refcount = refcount_incr(&it->refcount);
                    if (refcount == 2) { /* item is linked but not busy */
                        /* Double check ITEM_LINKED flag here, since we're
                         * past a memory barrier from the mutex. */
                        if ((it->it_flags & ITEM_LINKED) != 0) {
                            status = MOVE_FROM_LRU;
                        } else {
                            /* refcount == 1 + !ITEM_LINKED means the item is being
                             * uploaded to, or was just unlinked but hasn't been freed
                             * yet. Let it bleed off on its own and try again later */
                            status = MOVE_BUSY;
                        }
                    } else {
                        if (settings.verbose > 2) {
                            fprintf(stderr, "Slab reassign hit a busy item: refcount: %d (%d -> %d)\n",
                                it->refcount, slab_rebal.s_clsid, slab_rebal.d_clsid);
                        }
                        status = MOVE_BUSY;
                    }
                    /* Item lock must be held while modifying refcount */
                    if (status == MOVE_BUSY) {
                        refcount_decr(&it->refcount);
                        item_trylock_unlock(hold_lock);
                    }
                }
            }
        }

        switch (status) {
            case MOVE_FROM_LRU:
                /* Lock order is LRU locks -> slabs_lock. unlink uses LRU lock.
                 * We only need to hold the slabs_lock while initially looking
                 * at an item, and at this point we have an exclusive refcount
                 * (2) + the item is locked. Drop slabs lock, drop item to
                 * refcount 1 (just our own, then fall through and wipe it
                 */
                pthread_mutex_unlock(&slabs_lock);
                do_item_unlink(it, hv);
                item_trylock_unlock(hold_lock);
                pthread_mutex_lock(&slabs_lock);
            case MOVE_FROM_SLAB:
                it->refcount = 0;
                it->it_flags = 0;
                it->slabs_clsid = 255;
                break;
            case MOVE_BUSY:
            case MOVE_LOCKED:
                slab_rebal.busy_items++;
                was_busy++;
                break;
            case MOVE_PASS:
                break;
        }

        slab_rebal.slab_pos = (char *)slab_rebal.slab_pos + s_cls->size;
        if (slab_rebal.slab_pos >= slab_rebal.slab_end)
            break;
    }

    if (slab_rebal.slab_pos >= slab_rebal.slab_end) {
        /* Some items were busy, start again from the top */
        if (slab_rebal.busy_items) {
            slab_rebal.slab_pos = slab_rebal.slab_start;
            slab_rebal.busy_items = 0;
        } else {
            slab_rebal.done++;
        }
    }

    pthread_mutex_unlock(&slabs_lock);

    return was_busy;
}

static void slab_rebalance_finish(void) {
    slabclass_t *s_cls;
    slabclass_t *d_cls;

    pthread_mutex_lock(&slabs_lock);

    s_cls = &root->slabclass[slab_rebal.s_clsid];
    d_cls   = &root->slabclass[slab_rebal.d_clsid];

    /* At this point the stolen slab is completely clear */
    D_RW(s_cls->slab_list)[s_cls->killing - 1] =
         D_RW(s_cls->slab_list)[s_cls->slabs - 1];
    s_cls->slabs--;
    s_cls->killing = 0;

    memset(slab_rebal.slab_start, 0, (size_t)settings.item_size_max);

    D_RW(d_cls->slab_list)[d_cls->slabs++] = slab_rebal.slab_start;
    split_slab_page_into_freelist((char*)slab_rebal.slab_start,
        slab_rebal.d_clsid);

    slab_rebal.done       = 0;
    slab_rebal.s_clsid    = 0;
    slab_rebal.d_clsid    = 0;
    slab_rebal.slab_start = NULL;
    slab_rebal.slab_end   = NULL;
    slab_rebal.slab_pos   = NULL;

    slab_rebalance_signal = 0;

    pthread_mutex_unlock(&slabs_lock);

    STATS_LOCK();
    stats.slab_reassign_running = false;
    stats.slabs_moved++;
    STATS_UNLOCK();

    if (settings.verbose > 1) {
        fprintf(stderr, "finished a slab move\n");
    }
}

/* Return 1 means a decision was reached.
 * Move to its own thread (created/destroyed as needed) once automover is more
 * complex.
 */
static int slab_automove_decision(int *src, int *dst) {
    static uint64_t evicted_old[MAX_NUMBER_OF_SLAB_CLASSES];
    static unsigned int slab_zeroes[MAX_NUMBER_OF_SLAB_CLASSES];
    static unsigned int slab_winner = 0;
    static unsigned int slab_wins   = 0;
    uint64_t evicted_new[MAX_NUMBER_OF_SLAB_CLASSES];
    uint64_t evicted_diff = 0;
    uint64_t evicted_max  = 0;
    unsigned int highest_slab = 0;
    unsigned int total_pages[MAX_NUMBER_OF_SLAB_CLASSES];
    int i;
    int source = 0;
    int dest = 0;
    static rel_time_t next_run;

    /* Run less frequently than the slabmove tester. */
    if (current_time >= next_run) {
        next_run = current_time + 10;
    } else {
        return 0;
    }

    item_stats_evictions(evicted_new);
    pthread_mutex_lock(&slabs_lock);
    for (i = POWER_SMALLEST; i < root->power_largest; i++) {
        total_pages[i] = root->slabclass[i].slabs;
    }
    pthread_mutex_unlock(&slabs_lock);

    /* Find a candidate source; something with zero evicts 3+ times */
    for (i = POWER_SMALLEST; i < root->power_largest; i++) {
        evicted_diff = evicted_new[i] - evicted_old[i];
        if (evicted_diff == 0 && total_pages[i] > 2) {
            slab_zeroes[i]++;
            if (source == 0 && slab_zeroes[i] >= 3)
                source = i;
        } else {
            slab_zeroes[i] = 0;
            if (evicted_diff > evicted_max) {
                evicted_max = evicted_diff;
                highest_slab = i;
            }
        }
        evicted_old[i] = evicted_new[i];
    }

    /* Pick a valid destination */
    if (slab_winner != 0 && slab_winner == highest_slab) {
        slab_wins++;
        if (slab_wins >= 3)
            dest = slab_winner;
    } else {
        slab_wins = 1;
        slab_winner = highest_slab;
    }

    if (source && dest) {
        *src = source;
        *dst = dest;
        return 1;
    }
    return 0;
}

/* Slab rebalancer thread.
 * Does not use spinlocks since it is not timing sensitive. Burn less CPU and
 * go to sleep if locks are contended
 */
static void *slab_maintenance_thread(void *arg) {
    int src, dest;

    while (do_run_slab_thread) {
        if (settings.slab_automove == 1) {
            if (slab_automove_decision(&src, &dest) == 1) {
                /* Blind to the return codes. It will retry on its own */
                slabs_reassign(src, dest);
            }
            sleep(1);
        } else {
            /* Don't wake as often if we're not enabled.
             * This is lazier than setting up a condition right now. */
            sleep(5);
        }
    }
    return NULL;
}

/* Slab mover thread.
 * Sits waiting for a condition to jump off and shovel some memory about
 */
static void *slab_rebalance_thread(void *arg) {
    int was_busy = 0;
    /* So we first pass into cond_wait with the mutex held */
    mutex_lock(&slabs_rebalance_lock);

    while (do_run_slab_rebalance_thread) {
        if (slab_rebalance_signal == 1) {
            if (slab_rebalance_start() < 0) {
                /* Handle errors with more specifity as required. */
                slab_rebalance_signal = 0;
            }

            was_busy = 0;
        } else if (slab_rebalance_signal && slab_rebal.slab_start != NULL) {
            was_busy = slab_rebalance_move();
        }

        if (slab_rebal.done) {
            slab_rebalance_finish();
        } else if (was_busy) {
            /* Stuck waiting for some items to unlock, so slow down a bit
             * to give them a chance to free up */
            usleep(50);
        }

        if (slab_rebalance_signal == 0) {
            /* always hold this lock while we're running */
            pthread_cond_wait(&slab_rebalance_cond, &slabs_rebalance_lock);
        }
    }
    return NULL;
}

/* Iterate at most once through the slab classes and pick a "random" source.
 * I like this better than calling rand() since rand() is slow enough that we
 * can just check all of the classes once instead.
 */
static int slabs_reassign_pick_any(int dst) {
    static int cur = POWER_SMALLEST - 1;
    int tries = root->power_largest - POWER_SMALLEST + 1;
    for (; tries > 0; tries--) {
        cur++;
        if (cur > root->power_largest)
            cur = POWER_SMALLEST;
        if (cur == dst)
            continue;
        if (root->slabclass[cur].slabs > 1) {
            return cur;
        }
    }
    return -1;
}

static enum reassign_result_type do_slabs_reassign(int src, int dst) {
    if (slab_rebalance_signal != 0)
        return REASSIGN_RUNNING;

    if (src == dst)
        return REASSIGN_SRC_DST_SAME;

    /* Special indicator to choose ourselves. */
    if (src == -1) {
        src = slabs_reassign_pick_any(dst);
        /* TODO: If we end up back at -1, return a new error type */
    }

    if (src < POWER_SMALLEST || src > root->power_largest ||
        dst < POWER_SMALLEST || dst > root->power_largest)
        return REASSIGN_BADCLASS;

    if (root->slabclass[src].slabs < 2)
        return REASSIGN_NOSPARE;

    slab_rebal.s_clsid = src;
    slab_rebal.d_clsid = dst;

    slab_rebalance_signal = 1;
    pthread_cond_signal(&slab_rebalance_cond);

    return REASSIGN_OK;
}

enum reassign_result_type slabs_reassign(int src, int dst) {
    enum reassign_result_type ret;
    if (pthread_mutex_trylock(&slabs_rebalance_lock) != 0) {
        return REASSIGN_RUNNING;
    }
    ret = do_slabs_reassign(src, dst);
    pthread_mutex_unlock(&slabs_rebalance_lock);
    return ret;
}

/* If we hold this lock, rebalancer can't wake up or move */
void slabs_rebalancer_pause(void) {
    pthread_mutex_lock(&slabs_rebalance_lock);
}

void slabs_rebalancer_resume(void) {
    pthread_mutex_unlock(&slabs_rebalance_lock);
}

static pthread_t maintenance_tid;
static pthread_t rebalance_tid;

int start_slab_maintenance_thread(void) {

    int ret;
    slab_rebalance_signal = 0;
    slab_rebal.slab_start = NULL;
    char *env = getenv("MEMCACHED_SLAB_BULK_CHECK");
    if (env != NULL) {
        slab_bulk_check = atoi(env);
        if (slab_bulk_check == 0) {
            slab_bulk_check = DEFAULT_SLAB_BULK_CHECK;
        }
    }

    if (pthread_cond_init(&slab_rebalance_cond, NULL) != 0) {
        fprintf(stderr, "Can't intiialize rebalance condition\n");
        return -1;
    }
    pthread_mutex_init(&slabs_rebalance_lock, NULL);

    if ((ret = pthread_create(&maintenance_tid, NULL,
                              slab_maintenance_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create slab maint thread: %s\n", strerror(ret));
        return -1;
    }
    if ((ret = pthread_create(&rebalance_tid, NULL,
                              slab_rebalance_thread, NULL)) != 0) {
        fprintf(stderr, "Can't create rebal thread: %s\n", strerror(ret));
        return -1;
    }
    return 0;
}

/* The maintenance thread is on a sleep/loop cycle, so it should join after a
 * short wait */
void stop_slab_maintenance_thread(void) {
    mutex_lock(&slabs_rebalance_lock);
    do_run_slab_thread = 0;
    do_run_slab_rebalance_thread = 0;
    pthread_cond_signal(&slab_rebalance_cond);
    pthread_mutex_unlock(&slabs_rebalance_lock);

    /* Wait for the maintenance thread to stop */
    pthread_join(maintenance_tid, NULL);
    pthread_join(rebalance_tid, NULL);
}
