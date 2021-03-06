#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <string.h>

#include "hashtable.h"
#include "hashtable_support_index.h"
#include "hashtable_support_op.h"

// TODO: refactor to merge the functions hashtable_support_op_search_key and
//       hashtable_support_op_search_key_or_create_new and reorganize the code

bool hashtable_support_op_search_key(
        volatile hashtable_data_t *hashtable_data,
        hashtable_key_data_t *key,
        hashtable_key_size_t key_size,
        hashtable_bucket_hash_t hash,
        hashtable_bucket_index_t *found_index,
        volatile hashtable_bucket_key_value_t **found_key_value) {
    volatile hashtable_key_data_t* found_bucket_key;
    hashtable_key_size_t found_bucket_key_max_compare_size;
    hashtable_bucket_index_t index_neighborhood_begin, index_neighborhood_end;
    bool found = false;

    hashtable_support_index_calculate_neighborhood_from_hash(
            hashtable_data->buckets_count,
            hash,
            hashtable_data->cachelines_to_probe,
            &index_neighborhood_begin,
            &index_neighborhood_end);

    for(hashtable_bucket_index_t index = index_neighborhood_begin; index <= index_neighborhood_end; index++) {
        HASHTABLE_MEMORY_FENCE_LOAD();

        if (hashtable_data->hashes[index] != hash) {
            continue;
        }

        volatile hashtable_bucket_key_value_t* found_bucket_key_value = &hashtable_data->keys_values[index];

        // The key may potentially change if the item is first deleted and then recreated, if it's inline it
        // doesn't really matter because the key will mismatch and the execution will continue but if the key is
        // stored externally and the allocated memory is freed it may crash.
        if (HASHTABLE_BUCKET_KEY_VALUE_HAS_FLAG(found_bucket_key_value->flags, HASHTABLE_BUCKET_KEY_VALUE_FLAG_KEY_INLINE)) {
            found_bucket_key = (volatile hashtable_key_data_t*)&found_bucket_key_value->inline_key.data;
            found_bucket_key_max_compare_size = HASHTABLE_INLINE_KEY_MAX_SIZE;
        } else {
            // TODO: The keys must be stored in an append only memory structure to avoid locking, memory can't be freed
            //       immediately after the bucket is freed because it can be in use and would cause a crash34567
            found_bucket_key = found_bucket_key_value->external_key.data;
            found_bucket_key_max_compare_size = found_bucket_key_value->external_key.size;
        }

        // Stop if hash found but bucket being filled, edge case because of parallelism, if marked as delete continue
        // and if key doesn't match continue as well. The order of the operations doesn't matter.
        if (HASHTABLE_BUCKET_KEY_VALUE_IS_EMPTY(found_bucket_key_value->flags)) {
            break;
        } else if (HASHTABLE_BUCKET_KEY_VALUE_HAS_FLAG(
                found_bucket_key_value->flags,
                HASHTABLE_BUCKET_KEY_VALUE_FLAG_DELETED)) {
            continue;
        } else if (strncmp(
                key,
                (const char *)found_bucket_key,
                MIN(found_bucket_key_max_compare_size, key_size)) != 0) {
            continue;
        }

        *found_index = index;
        *found_key_value = found_bucket_key_value;
        found = true;
        break;
    }

    return found;
}

hashtable_search_key_or_create_new_ret_t hashtable_support_op_search_key_or_create_new(
        volatile hashtable_data_t *hashtable_data,
        hashtable_key_data_t *key,
        hashtable_key_size_t key_size,
        hashtable_bucket_hash_t hash,
        bool *created_new,
        hashtable_bucket_index_t *found_index,
        volatile hashtable_bucket_key_value_t **found_key_value) {
    volatile hashtable_key_data_t* found_bucket_key;
    hashtable_key_size_t found_bucket_key_max_compare_size;
    hashtable_bucket_index_t index_neighborhood_begin, index_neighborhood_end;
    hashtable_search_key_or_create_new_ret_t ret = HASHTABLE_SEARCH_KEY_OR_CREATE_NEW_RET_NO_FREE;

    hashtable_support_index_calculate_neighborhood_from_hash(
            hashtable_data->buckets_count,
            hash,
            hashtable_data->cachelines_to_probe,
            &index_neighborhood_begin,
            &index_neighborhood_end);

    bool terminate_outer_loop = false;
    for(uint8_t searching_or_creating = 0; searching_or_creating < 2; searching_or_creating++) {
        for(hashtable_bucket_index_t index = index_neighborhood_begin; index <= index_neighborhood_end; index++) {
            HASHTABLE_MEMORY_FENCE_LOAD();

            if (searching_or_creating == 0) {
                // If it's searching, loop of the neighborhood searching for the hash
                if (hashtable_data->hashes[index] != hash) {
                    continue;
                }

            } else if (searching_or_creating == 1) {
                // If it's creating, it has still to search not only an empty bucket but a bucket with the key as well
                // because it may have been created in the mean time
                if (hashtable_data->hashes[index] != hash && hashtable_data->hashes[index] != 0) {
                    continue;
                }

                hashtable_bucket_hash_t expected_hash = 0U;

                // If the operation is successful, it's a new bucket, if it fails it may be an existing bucket, this
                // specific case is checked below
                *created_new = atomic_compare_exchange_strong(&hashtable_data->hashes[index], &expected_hash, hash);

                if (*created_new == false) {
                    // Corner case, consider valid the operation if the new hash is what was going to be set
                    if (expected_hash != hash) {
                        continue;
                    }
                }
            }

            volatile hashtable_bucket_key_value_t* found_bucket_key_value = &hashtable_data->keys_values[index];

            if (searching_or_creating == 0 || *created_new == false) {
                // The flags are set as very last information so it's safe to assume that if they are empty there is a
                // set that is creating a bucket ran by another thread that hasn't finished yet.
                // If it's marked as deleted instead, it means that the bucket was containing the right hash but has
                // been GCed or deleted and there fore it's pointless to set the new value because the intent was to
                // remove this specific key/value.
                if (
                        HASHTABLE_BUCKET_KEY_VALUE_IS_EMPTY(found_bucket_key_value->flags)
                        ||
                        HASHTABLE_BUCKET_KEY_VALUE_HAS_FLAG(
                                found_bucket_key_value->flags,
                                HASHTABLE_BUCKET_KEY_VALUE_FLAG_DELETED)) {
                    terminate_outer_loop = true;
                    ret = HASHTABLE_SEARCH_KEY_OR_CREATE_NEW_RET_EMPTY_OR_DELETED;
                    break;
                }

                if (HASHTABLE_BUCKET_KEY_VALUE_HAS_FLAG(
                        found_bucket_key_value->flags,
                        HASHTABLE_BUCKET_KEY_VALUE_FLAG_KEY_INLINE)) {
                    found_bucket_key = (volatile hashtable_key_data_t*)&found_bucket_key_value->inline_key.data;
                    found_bucket_key_max_compare_size = HASHTABLE_INLINE_KEY_MAX_SIZE;
                } else {
                    found_bucket_key = found_bucket_key_value->external_key.data;
                    found_bucket_key_max_compare_size = found_bucket_key_value->external_key.size;
                }

                int res = strncmp(key, (const char *)found_bucket_key, MIN(found_bucket_key_max_compare_size, key_size));
                if (res != 0) {
                    continue;
                }
            }

            HASHTABLE_MEMORY_FENCE_STORE();

            *found_index = index;
            *found_key_value = found_bucket_key_value;
            ret = HASHTABLE_SEARCH_KEY_OR_CREATE_NEW_RET_FOUND;
            terminate_outer_loop = true;
            break;
        }

        if (terminate_outer_loop) {
            break;
        }
    }

    return ret;
}
