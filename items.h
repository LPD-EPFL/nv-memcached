/* See items.c */
uint64_t get_cas_id(void);

#ifdef NVM

static uint64_t* volatile timestamps;
static uint64_t* volatile lastCollectEpochs;
static __thread uint64_t* my_timestamp;
static __thread int my_id;
#define ITEM_TIMESTAMP ((*my_timestamp)++)
static __thread active_slab_table_t* slab_table;
static active_slab_table_t** slab_tables;


 uint64_t getMyTimestamp(); 
 uint64_t getMyLastCollect();
 active_slab_table_t* getMySlabTable(); 

void item_gc_init(unsigned int size_limit, int num_threads);
void item_gc_thread_init(int thread_id);
void recover();
#endif

/*@null@*/
item *do_item_alloc(char *key, const size_t nkey, const int flags, const rel_time_t exptime, const int nbytes, const uint32_t cur_hv);
void item_free(item *it);
bool item_size_ok(const size_t nkey, const int flags, const int nbytes);

int  do_item_link(item *it, const uint32_t hv);     /** may fail if transgresses limits */
void do_item_unlink(item *it, const uint32_t hv);
void do_item_unlink_nolock(item *it, const uint32_t hv);
void do_item_remove(item *it);
void do_item_update(item *it);   /** update LRU time to current and reposition */
void do_item_update_nolock(item *it);
int  do_item_replace(item *it, item *new_it, const uint32_t hv);
#ifdef NVM
void do_item_set(item *it, const uint32_t hv);
int  do_item_add(item *it, const uint32_t hv);
#endif

/*@null@*/
char *item_cachedump(const unsigned int slabs_clsid, const unsigned int limit, unsigned int *bytes);
void item_stats(ADD_STAT add_stats, void *c);
void item_stats_totals(ADD_STAT add_stats, void *c);
/*@null@*/
void item_stats_sizes(ADD_STAT add_stats, void *c);

item *do_item_get(const char *key, const size_t nkey, const uint32_t hv);
item *do_item_touch(const char *key, const size_t nkey, uint32_t exptime, const uint32_t hv);
#ifdef NVM
void do_item_release(item* it);
#endif
void item_stats_reset(void);
extern pthread_mutex_t lru_locks[POWER_LARGEST];
void item_stats_evictions(uint64_t *evicted);

enum crawler_result_type {
    CRAWLER_OK=0, CRAWLER_RUNNING, CRAWLER_BADCLASS, CRAWLER_NOTSTARTED
};

int start_lru_maintainer_thread(void);
int stop_lru_maintainer_thread(void);
int init_lru_maintainer(void);
void lru_maintainer_pause(void);
void lru_maintainer_resume(void);

int start_item_crawler_thread(void);
int stop_item_crawler_thread(void);
int init_lru_crawler(void);
enum crawler_result_type lru_crawler_crawl(char *slabs);
void lru_crawler_pause(void);
void lru_crawler_resume(void);
