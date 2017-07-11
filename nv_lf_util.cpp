#include <string.h>
#include "nv_lf_util.h"
#include "memcached.h"
#include "common.h"


int keycmp_key_item(const char* key, const size_t nkey, svalue_t item_ptr)
{
    // TODO: implement fast key compare function
    item* it = (item*)item_ptr;
    const size_t min_len = (nkey < it->nkey) ? nkey : it->nkey;
    int r = memcmp(key, ITEM_key(it), min_len);
    if (r == 0) {
        if (nkey < it->nkey) r = -1;
        else if (nkey > it->nkey) r = +1;
        }
    return r;
}

int keycmp_item_item(svalue_t item_ptr1, svalue_t item_ptr2)
{
    // TODO: implement fast key compare function
    item* it1 = (item*)item_ptr1;
    item* it2 = (item*)item_ptr2;
    const size_t min_len = (it1->nkey < it2->nkey) ? it1->nkey : it2->nkey;
    int r = memcmp(ITEM_key(it1), ITEM_key(it2), min_len);
    if (r == 0) {
        if (it1->nkey < it2->nkey) r = -1;
        else if (it1->nkey > it2->nkey) r = +1;
        }
    return r;
}