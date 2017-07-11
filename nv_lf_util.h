#ifndef _NV_LF_UTIL_H_
#define _NV_LF_UTIL_H_

#include "common.h"
/* Key comparison of memcached items for clht hashtable
 *
 * key         key to compare
 * nkey        key length
 * item_ptr    stored item to compare key with
 */
int keycmp_key_item(const char* key, const size_t nkey, svalue_t item_ptr);
int keycmp_item_item(svalue_t item_ptr1, svalue_t item_ptr2);

#endif
