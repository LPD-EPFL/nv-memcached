/*   
 *   File: intset.c
 *   Author: Vincent Gramoli <vincent.gramoli@sydney.edu.au>, 
 *  	     Vasileios Trigonakis <vasileios.trigonakis@epfl.ch>
 *   Description: 
 *   intset.c is part of ASCYLIB
 *
 * Copyright (c) 2014 Vasileios Trigonakis <vasileios.trigonakis@epfl.ch>,
 * 	     	      Tudor David <tudor.david@epfl.ch>
 *	      	      Distributed Programming Lab (LPD), EPFL
 *
 * ASCYLIB is free software: you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, version 2
 * of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include "memcached.h"

svalue_t
ht_contains(ht_intset_t* set, skey_t key, const char* full_key, const size_t nkey, EpochThread epoch, linkcache_t* buffer) {

  int addr = key & set->hash;
  return linkedlist_find(&set->buckets[addr], key, full_key, nkey, epoch, buffer);
}

svalue_t 
ht_add(ht_intset_t* set, skey_t key, svalue_t val, int replace, EpochThread epoch, linkcache_t* buffer)
{
  int addr = key & set->hash;
  return linkedlist_insert(&set->buckets[addr], key, val, replace, epoch, buffer);
}

svalue_t
ht_remove(ht_intset_t* set, skey_t key, const char* full_key, const size_t nkey, EpochThread epoch, linkcache_t* buffer)
{
  int addr = key & set->hash;
  return linkedlist_remove(&set->buckets[addr], key, full_key, nkey, epoch, buffer);
}


int is_reachable(ht_intset_t* ht, void* address) {
    skey_t key = ((node_t*) address)->key;
    int addr = key & ht->hash;
    linkedlist_t* ll = &ht->buckets[addr];

    volatile node_t* prev = (*ll);
    volatile node_t* node = (node_t*)unmark_ptr_cache((uintptr_t)(*ll)->next);
    
    while (node->key < key) {
        prev = node;
        node = UNMARKED_PTR(node->next);
        node= (volatile node_t*)unmark_ptr_cache((UINT_PTR)node);
    }
    
    if ((node->key == key) && ((void*)node == address)) {
        return 1;
    }
    return 0;
}

int item_is_reachable(ht_intset_t* ht, void* it) {
    item* my_item = (item*)it;
    // get the hashvalue (ht key) of the item
    uint32_t hv = hash(ITEM_key(my_item), my_item->nkey);

    // compute the bucket where this item would be
    int addr = hv & ht->hash;
    linkedlist_t* ll = &ht->buckets[addr];

    // search for the item in the bucket
    if (linkedlist_find_simple(ll, hv, ITEM_key(my_item), my_item->nkey) == 0) {
        return 0;
    } else {
        return 1;
    }
}

void ht_recover(ht_intset_t* ht, active_page_table_t** page_buffers, int num_page_buffers) {
        // now go over all the pages in the page buffers and check which of the nodes there are reachable;

    // first, remove the marked nodes of each linked list
    //node_t ** unlinking_address = (node_t**)EpochCacheAlignedAlloc(sizeof(node_t*));
    linkedlist_t* ll;
    volatile node_t* prev; 
    volatile node_t* node; 
    volatile node_t* next;
    int i;
    size_t j;

    //for (j = 0; j < maxhtlength; j++) {
        //ll = &ht->buckets[j];

        //prev = UNMARKED_PTR((*ll));
        //node = UNMARKED_PTR((*ll)->next);

        ////remove the marked nodes
        //while (node->next != NULL) {

            //next = UNMARKED_PTR(node->next);

            //if (PTR_IS_MARKED(node->next)) {
                //*unlinking_address = (node_t*)node;
                //write_data_wait(unlinking_address, 1);
                //prev->next = next;
                //write_data_wait((void*)prev, CACHE_LINES_PER_NV_NODE);  
                //if (!NodeMemoryIsFree((void*)node)) {
                    //finalize_node((void*)node, NULL, NULL);
                //}
                //node = prev->next;
            //}
            //else {
                //prev = node;
                //node = next;
            //}
        //}
        //wait_writes();
    //}
    //EpochCacheAlignedFree(unlinking_address);


    size_t k;
    size_t page_size;
    size_t nodes_per_page;

    page_descriptor_t* crt;
    size_t num_pages;

    //fprintf(stderr, "recovery going over pages\n");

    for (i = 0; i < num_page_buffers; i++) {
        //fprintf(stderr, "going over page table %d\n",i);
        page_size = page_buffers[i]->page_size; //TODO: now assuming all the pages in the buffer have one size; change this? (given that in the NV heap we basically just use one page size (except the bottom level), should be fine)
        num_pages = page_buffers[i]->last_in_use;
        crt = page_buffers[i]->pages;
        for (j = 0; j < num_pages; j++) {
            if (crt[j].page != NULL) {
                void * crt_address = crt[j].page;
                nodes_per_page = page_size / sizeof(node_t);
                for (k = 0; k < nodes_per_page; k++) {
                    void * node_address = (void*)((UINT_PTR)crt_address + (CACHE_LINES_PER_NV_NODE*CACHE_LINE_SIZE*k));
                    if (!NodeMemoryIsFree(node_address)) {
                        if (!is_reachable(ht, node_address)) {
    
                            MarkNodeMemoryAsFree(node_address); //if a node is not reachable but its memory is marked as allocated, need to free the node
                        //  }
                    }
                    }
                }
            }
        }
        //fprintf(stderr, "destroying page table %d\n",i);
        //destroy_active_page_table(page_buffers[i]);
        }
}
