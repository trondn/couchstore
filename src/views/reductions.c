/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */

#include "reductions.h"
#include "../bitfield.h"
#include "../couch_btree.h"
#include <stdlib.h>
#include <string.h>

#define BITMASK_BYTE_SIZE      (1024 / CHAR_BIT)

#define dec_uint16(b) (decode_raw16(*((raw_16 *) b)))
#define dec_uint48(b) (decode_raw48(*((raw_48 *) b)))
#define dec_uint40(b) (decode_raw40(*((raw_40 *) b)))

static void enc_uint16(uint16_t u, char **buf);

static void enc_raw40(uint64_t u, char **buf);


couchstore_error_t decode_view_btree_reduction(const char *bytes,
                                               size_t len,
                                               view_btree_reduction_t **reduction)
{
    view_btree_reduction_t *r = NULL;
    uint8_t  i, j;
    uint16_t sz;
    const char *bs;
    size_t length;

    r = (view_btree_reduction_t *) malloc(sizeof(view_btree_reduction_t));
    if (r == NULL) {
        goto alloc_error;
    }

    r->reduce_values = NULL;

    assert(len >= 5);
    r->kv_count = dec_uint40(bytes);
    bytes += 5;
    len -= 5;

    assert(len >= BITMASK_BYTE_SIZE);
    memcpy(&r->partitions_bitmap, bytes, BITMASK_BYTE_SIZE);
    bytes += BITMASK_BYTE_SIZE;
    len -= BITMASK_BYTE_SIZE;

    bs = bytes;
    length = len;

    r->num_values = 0;
    while (len > 0) {

        assert(len >= 2);
        sz = dec_uint16(bs);
        bs += 2;
        len -= 2;

        assert(len >= sz);
        bs += sz;
        len -= sz;
        r->num_values++;
    }


    if (len > 0) {
        free_view_btree_reduction(r);
        return COUCHSTORE_ERROR_CORRUPT;
    }

    r->reduce_values = (sized_buf *) malloc(r->num_values * sizeof(sized_buf));

    if (r->reduce_values == NULL) {
        goto alloc_error;
    }

    for (j = 0; j< r->num_values; ++j) {
        r->reduce_values[j].buf = NULL;
    }

    i = 0;
    len = length;
    while (len > 0) {

        sz = dec_uint16(bytes);
        bytes += 2;
        len -= 2;

        r->reduce_values[i].size = sz;
        r->reduce_values[i].buf = (char *) malloc(sz);

        if (r->reduce_values[i].buf == NULL) {
            goto alloc_error;
        }

        memcpy(r->reduce_values[i].buf, bytes, sz);
        bytes += sz;
        len -= sz;
        i++;
    }

    *reduction = r;

    return COUCHSTORE_SUCCESS;

 alloc_error:
    free_view_btree_reduction(r);
    return COUCHSTORE_ERROR_ALLOC_FAIL;
}


couchstore_error_t encode_view_btree_reduction(const view_btree_reduction_t *reduction,
                                               char *buffer,
                                               size_t *buffer_size)
{
    char *b = NULL;
    size_t sz = 0;

    sz += 5;                     /* kv_count */
    sz += BITMASK_BYTE_SIZE; /* partitions bitmap */
    /* reduce values */
    for (int i = 0; i < reduction->num_values; ++i) {
        sz += 2;             /* size_t */
        sz += reduction->reduce_values[i].size;
    }

    if (sz > MAX_REDUCTION_SIZE) {
        return COUCHSTORE_ERROR_REDUCTION_TOO_LARGE;
    }

    b = buffer;

    enc_raw40(reduction->kv_count, &b);

    memcpy(b, &reduction->partitions_bitmap, BITMASK_BYTE_SIZE);
    b += BITMASK_BYTE_SIZE;

    for (int i = 0; i < reduction->num_values; ++i) {
        enc_uint16(reduction->reduce_values[i].size, &b);

        memcpy(b, reduction->reduce_values[i].buf, reduction->reduce_values[i].size);
        b += reduction->reduce_values[i].size;
    }

    *buffer_size = sz;

    return COUCHSTORE_SUCCESS;
}


void free_view_btree_reduction(view_btree_reduction_t *reduction)
{
    if (reduction == NULL) {
        return;
    }

    if (reduction->reduce_values != NULL){
        for (int i = 0; i < reduction->num_values; ++i) {
            free(reduction->reduce_values[i].buf);
        }
        free(reduction->reduce_values);
    }

    free(reduction);
}


couchstore_error_t decode_view_id_btree_reduction(const char *bytes,
                                                  view_id_btree_reduction_t **reduction)
{
    view_id_btree_reduction_t *r = NULL;

    r = (view_id_btree_reduction_t *) malloc(sizeof(view_id_btree_reduction_t));
    if (r == NULL) {
        goto alloc_error;
    }

    r->kv_count = dec_uint40(bytes);
    bytes += 5;

    memcpy(&r->partitions_bitmap, bytes, BITMASK_BYTE_SIZE);

    *reduction = r;

    return COUCHSTORE_SUCCESS;

 alloc_error:
    free_view_id_btree_reduction(r);
    return COUCHSTORE_ERROR_ALLOC_FAIL;
}


couchstore_error_t encode_view_id_btree_reduction(const view_id_btree_reduction_t *reduction,
                                                  char *buffer,
                                                  size_t *buffer_size)
{
    char *b = NULL;
    size_t sz = 0;

    sz += 5;                     /* kv_count */
    sz += BITMASK_BYTE_SIZE; /* partitions bitmap */

    if (sz > MAX_REDUCTION_SIZE) {
        return COUCHSTORE_ERROR_REDUCTION_TOO_LARGE;
    }

    b = buffer;

    enc_raw40(reduction->kv_count, &b);

    memcpy(b, &reduction->partitions_bitmap, BITMASK_BYTE_SIZE);

    *buffer_size = sz;

    return COUCHSTORE_SUCCESS;
}


void free_view_id_btree_reduction(view_id_btree_reduction_t *reduction)
{
    if (reduction == NULL) {
        return;
    }

    free(reduction);
}

static void enc_uint16(uint16_t u, char **buf)
{
    raw_16 r = encode_raw16(u);
    memcpy(*buf, &r, 2);
    *buf += 2;
}


static void enc_raw40(uint64_t u, char **buf)
{
    raw_40 r = encode_raw40(u);
    memcpy(*buf, &r, 5);
    *buf += 5;
}

