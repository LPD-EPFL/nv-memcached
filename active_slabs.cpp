//#include "epoch_impl.h"

#include "active_slabs.h"
#include "memcached.h"


__thread char slabs_path[32];

static __thread PMEMobjpool *pop;

active_slab_table_t* allocate_ast(uint32_t id) {

    //char path[32];
    sprintf(slabs_path, "/tmp/slabs_thread_%u", id); //thread id as file name

    //remove file if it exists
    //TODO might want to remove this instruction in the future
    remove(slabs_path);

    pop = NULL;

    if (access(slabs_path, F_OK) != 0) {
        if ((pop = pmemobj_create(slabs_path, POBJ_LAYOUT_NAME(ast),
            AST_POOL_SIZE, S_IWUSR | S_IRUSR)) == NULL) {
            printf("failed to create pool1 wiht name %s\n", slabs_path);
            return NULL;
        }
    } else {
        if ((pop = pmemobj_open(slabs_path, LAYOUT_NAME)) == NULL) {
            printf("failed to open pool with name %s\n", slabs_path);
            return NULL;
        }
    }
    
    //zeroed allocation if the object does not exist yet
    TOID(active_slab_table_t) ast = POBJ_ROOT(pop, active_slab_table_t);

    //I should now return a pointer to this 
    return D_RW(ast);
}

/*
    creates a slab buffer with a certain number of preallocated free entries
*/
active_slab_table_t* create_active_slab_table(uint32_t id) {

    active_slab_table_t* new_buffer = NULL;

    new_buffer = allocate_ast(id); //zeroed allocation

    new_buffer->current_size = 0;

    new_buffer->last_in_use = DEFAULT_SLAB_BUFFER_SIZE;
    write_data_nowait(new_buffer, 1);

    wait_writes();
    return new_buffer;
}

/*
    frees all the memory associated with a slab buffer
*/
void destroy_active_slab_table(active_slab_table_t* active_slab_table) {
    
    pmemobj_close(pop);

    remove(slabs_path);
}

/*
    clears all the entries in the buffer;
*/
void clear_buffer(active_slab_table_t* buffer, uint64_t cleanTs, uint64_t currTs) {

    size_t max_seen = 0;
    size_t i;

    for (i = 0; i < buffer->last_in_use; i++) {
        if ((buffer->slabs[i].slab!=NULL) && ((buffer->slabs[i].lastUnlinkEpoch < cleanTs) || (buffer->slabs[i].lastUnlinkEpoch ==0)) && ((buffer->slabs[i].lastAllocEpoch < currTs) || (buffer->slabs[i].lastAllocEpoch == 0))) {
            buffer->slabs[i].slab = NULL;
            buffer->slabs[i].lastUnlinkEpoch = 0;
            buffer->current_size--;
        }
        if ((buffer->slabs[i].slab != NULL) && (i > max_seen)) {
            max_seen = i;
        }   
    }

    //decrease search size if last half is empty
    size_t half = buffer->last_in_use/2;
    if ((max_seen < half) && (half > DEFAULT_SLAB_BUFFER_SIZE)) {
       buffer->last_in_use = half;
    }

    buffer->clear_all = 0;
    // no need to persist this now
}

/*
    mark a slab as having data that was either allocated or freed in the current epoch
*/

void mark_slab(active_slab_table_t* slabs, void* ptr, void* slab, uint64_t currentTs, uint64_t collectTs, int isRemove) {


    if ((slabs->clear_all) || (slabs->current_size > CLEAN_THRESHOLD)) {
        //fprintf(stderr, "clear all size before %u curr ts %u collect ts %u\n", slabs->current_size, currentTs, collectTs);
        clear_buffer(slabs, collectTs, currentTs);
        //fprintf(stderr, "clear all size after %u\n", slabs->current_size);
    }

    item* address = (item*)ptr;
    // if (address == NULL) {
    //     address = GetNextNodeAddress(allocation_size);
    // }

    // void* slab = get_slab_start_address(address);

    size_t i;

    size_t first_empty = SIZE_MAX;


    for (i = 0; i < slabs->last_in_use; i++) {
            if (slabs->slabs[i].slab == slab) {
                //slab already present, nothing to add, can return

                if (isRemove) {
                    if (slabs->slabs[i].lastUnlinkEpoch < currentTs) {
                        slabs->slabs[i].lastUnlinkEpoch = currentTs;
                        //no need to persist this, the timestamps are not important for recovery
                    }
                }
                else {
                    if (slabs->slabs[i].lastAllocEpoch < currentTs) {
                        slabs->slabs[i].lastAllocEpoch = currentTs;
                        //no need to persist this, the timestamps are not important for recovery
                    }
                }
                return;
            }

            if (slabs->slabs[i].slab == NULL) {
                if (first_empty == SIZE_MAX) first_empty = i;
            }
    }

    if (first_empty != SIZE_MAX){ 
        slabs->slabs[first_empty].slab = slab;
        if (isRemove) {
            slabs->slabs[first_empty].lastUnlinkEpoch = currentTs;
            slabs->slabs[first_empty].lastAllocEpoch = 0;
        }
        else {
            slabs->slabs[first_empty].lastUnlinkEpoch = 0;
            slabs->slabs[first_empty].lastAllocEpoch = currentTs;
        }
        slabs->current_size++;
        
        write_data_wait(&(slabs->slabs[first_empty]), 1);
        return;
    }


    // slab has not been found, and no empty entry in the buffer, up to last_in_use, means we need to try to expand our search space
    size_t twice = slabs->last_in_use*2; 

    if (twice >= MAX_NUM_SLABS) {
        fprintf(stderr, "SLAB_BUFFER_SIZE_EXCEEDED!\n");
        return;
    }

    size_t old = slabs->last_in_use;

    slabs->last_in_use = twice;
    write_data_wait(slabs,1); //need to make sure this is persisted before writing after the marker

    assert(slabs->slabs[old].slab == NULL); //we just expanded; this means the newly enabled slab entries should be null

    slabs->slabs[old].slab = slab;
    if (isRemove) {
        slabs->slabs[old].lastUnlinkEpoch = currentTs;
        slabs->slabs[old].lastAllocEpoch = 0;
    }
    else {
        slabs->slabs[old].lastUnlinkEpoch = 0;
        slabs->slabs[old].lastAllocEpoch = currentTs;
    }

    write_data_nowait(&(slabs->slabs[old]), 1);

    wait_writes();

    slabs->current_size++;

}
