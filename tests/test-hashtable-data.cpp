#include "catch.hpp"

#include "hashtable/hashtable.h"
#include "hashtable/hashtable_data.h"
#include "hashtable/hashtable_support_primenumbers.h"

#include "fixtures-hashtable.h"

TEST_CASE("hashtable_data.c", "[hashtable][hashtable_data]") {
    SECTION("hashtable_data_init") {
        HASHTABLE_DATA(buckets_initial_count_5, {
            /* do nothing */
        })
    }

    SECTION("hashtable_data_init - mmap zero'ed") {
        HASHTABLE_DATA(buckets_initial_count_305, {
            for(
                    hashtable_bucket_count_t bucket_index = 0;
                    bucket_index < hashtable_data->buckets_count_real;
                    bucket_index++) {
                REQUIRE(hashtable_data->hashes[bucket_index] == 0);
            }
        })
    }

    SECTION("hashtable_data->buckets_count_real") {
        HASHTABLE_DATA(buckets_initial_count_5, {
            REQUIRE(hashtable_data->buckets_count_real == buckets_count_real_32);
        })

        HASHTABLE_DATA(buckets_initial_count_100, {
            REQUIRE(hashtable_data->buckets_count_real == buckets_count_real_128);
        })

        HASHTABLE_DATA(buckets_initial_count_305, {
            REQUIRE(hashtable_data->buckets_count_real == buckets_count_real_336);
        })
    }

    SECTION("invalid buckets_count") {
        hashtable_data_t *hashtable_data;

        hashtable_data = hashtable_data_init(HASHTABLE_PRIMENUMBERS_MAX + 1, cachelines_to_probe_2);

        REQUIRE(hashtable_data == NULL);
    }
}
