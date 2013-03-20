/* -*- Mode: C; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include <string.h>
#include <stddef.h>
#include <stdlib.h>

#include "internal.h"
#include "node_types.h"
#include "util.h"
#include "reduces.h"

static size_t assemble_seq_index_value(DocInfo *docinfo, char *dst)
{
    char* const start = dst;
    raw_seq_index_value *raw = (raw_seq_index_value*)dst;
    raw->sizes = encode_kv_length(docinfo->id.size, docinfo->size);
    raw->bp = encode_raw48(docinfo->bp | (docinfo->deleted ? 1LL<<47 : 0));
    raw->content_meta = encode_raw08(docinfo->content_meta);
    raw->rev_seq = encode_raw48(docinfo->rev_seq);
    dst += sizeof(*raw);

    memcpy(dst, docinfo->id.buf, docinfo->id.size);
    dst += docinfo->id.size;
    memcpy(dst, docinfo->rev_meta.buf, docinfo->rev_meta.size);
    dst += docinfo->rev_meta.size;
    return dst - start;
}

static size_t assemble_id_index_value(DocInfo *docinfo, char *dst)
{
    char* const start = dst;
    raw_id_index_value *raw = (raw_id_index_value*)dst;
    raw->db_seq = encode_raw48(docinfo->db_seq);
    raw->size = encode_raw32((uint32_t)docinfo->size);
    raw->bp = encode_raw48(docinfo->bp | (docinfo->deleted ? 1LL<<47 : 0));
    raw->content_meta = encode_raw08(docinfo->content_meta);
    raw->rev_seq = encode_raw48(docinfo->rev_seq);
    dst += sizeof(*raw);

    memcpy(dst, docinfo->rev_meta.buf, docinfo->rev_meta.size);
    dst += docinfo->rev_meta.size;
    return dst - start;
}

static couchstore_error_t write_doc(Db *db, const Doc *doc, uint64_t *bp,
                                    size_t* disk_size, couchstore_save_options writeopts)
{
    couchstore_error_t errcode;
    if (writeopts & COMPRESS_DOC_BODIES) {
        errcode = db_write_buf_compressed(db, &doc->data, (cs_off_t *) bp, disk_size);
    } else {
        errcode = db_write_buf(db, &doc->data, (cs_off_t *) bp, disk_size);
    }

    return errcode;
}

static int ebin_ptr_compare(const void *a, const void *b)
{
    const sized_buf* const* buf1 = a;
    const sized_buf* const* buf2 = b;
    return ebin_cmp(*buf1, *buf2);
}

static int seq_action_compare(const void *actv1, const void *actv2)
{
    const couchfile_modify_action *act1, *act2;
    act1 = (const couchfile_modify_action *) actv1;
    act2 = (const couchfile_modify_action *) actv2;

    uint64_t seq1, seq2;

    seq1 = decode_sequence_key(act1->key);
    seq2 = decode_sequence_key(act2->key);

    if (seq1 < seq2) {
        return -1;
    }
    if (seq1 == seq2) {
        if (act1->type < act2->type) {
            return -1;
        }
        if (act1->type > act2->type) {
            return 1;
        }
        return 0;
    }
    if (seq1 > seq2) {
        return 1;
    }
    return 0;
}

typedef struct _idxupdatectx {
    couchfile_modify_action *seqacts;
    int actpos;

    sized_buf **seqs;
    sized_buf **seqvals;
    int valpos;

    fatbuf *deltermbuf;
} index_update_ctx;

static void idfetch_update_cb(couchfile_modify_request *rq,
                              sized_buf *k, sized_buf *v, void *arg)
{
    (void)k;
    (void)rq;
    //v contains a seq we need to remove ( {Seq,_,_,_,_} )
    uint64_t oldseq;
    sized_buf *delbuf = NULL;
    index_update_ctx *ctx = (index_update_ctx *) arg;

    if (v == NULL) { //Doc not found
        return;
    }

    const raw_id_index_value *raw = (raw_id_index_value*) v->buf;
    oldseq = decode_raw48(raw->db_seq);

    delbuf = (sized_buf *) fatbuf_get(ctx->deltermbuf, sizeof(sized_buf));
    delbuf->buf = (char *) fatbuf_get(ctx->deltermbuf, 6);
    delbuf->size = 6;
    memset(delbuf->buf, 0, 6);
    *(raw_48*)delbuf->buf = encode_raw48(oldseq);

    ctx->seqacts[ctx->actpos].type = ACTION_REMOVE;
    ctx->seqacts[ctx->actpos].value.data = NULL;
    ctx->seqacts[ctx->actpos].key = delbuf;

    ctx->actpos++;
}

static couchstore_error_t update_indexes(Db *db,
                                         sized_buf *seqs,
                                         sized_buf *seqvals,
                                         sized_buf *ids,
                                         sized_buf *idvals,
                                         int numdocs)
{
    couchfile_modify_action *idacts;
    couchfile_modify_action *seqacts;
    const sized_buf **sorted_ids = NULL;
    size_t size;
    fatbuf *actbuf;
    node_pointer *new_id_root;
    node_pointer *new_seq_root;
    couchstore_error_t errcode;
    couchfile_modify_request seqrq, idrq;
    sized_buf tmpsb;
    int ii;

    /*
    ** Two action list up to numdocs * 2 in size + Compare keys for ids,
    ** and compare keys for removed seqs found from id index +
    ** Max size of a int64 erlang term (for deleted seqs)
    */
    size = 4 * sizeof(couchfile_modify_action) + 2 * sizeof(sized_buf) + 10;

    actbuf = fatbuf_alloc(numdocs * size);
    error_unless(actbuf, COUCHSTORE_ERROR_ALLOC_FAIL);

    idacts = fatbuf_get(actbuf, numdocs * sizeof(couchfile_modify_action) * 2);
    seqacts = fatbuf_get(actbuf, numdocs * sizeof(couchfile_modify_action) * 2);
    error_unless(idacts && seqacts, COUCHSTORE_ERROR_ALLOC_FAIL);

    index_update_ctx fetcharg = {
        seqacts, 0, &seqs, &seqvals, 0, actbuf
    };

    // Sort the array indexes of ids[] by ascending id. Since we can't pass context info to qsort,
    // actually sort an array of pointers to the elements of ids[], rather than the array indexes.
    sorted_ids = malloc(numdocs * sizeof(sized_buf*));
    error_unless(sorted_ids, COUCHSTORE_ERROR_ALLOC_FAIL);
    for (ii = 0; ii < numdocs; ++ii) {
        sorted_ids[ii] = &ids[ii];
    }
    qsort(sorted_ids, numdocs, sizeof(sorted_ids[0]), &ebin_ptr_compare);

    // Assemble idacts[] array, in sorted order by id:
    for (ii = 0; ii < numdocs; ii++) {
        ptrdiff_t isorted = sorted_ids[ii] - ids;   // recover index of ii'th id in sort order

        idacts[ii * 2].type = ACTION_FETCH;
        idacts[ii * 2].value.arg = &fetcharg;
        idacts[ii * 2 + 1].type = ACTION_INSERT;
        idacts[ii * 2 + 1].value.data = &idvals[isorted];
        idacts[ii * 2].key = &ids[isorted];
        idacts[ii * 2 + 1].key = &ids[isorted];
    }

    idrq.cmp.compare = ebin_cmp;
    idrq.cmp.arg = &tmpsb;
    idrq.db = db;
    idrq.actions = idacts;
    idrq.num_actions = numdocs * 2;
    idrq.reduce = by_id_reduce;
    idrq.rereduce = by_id_rereduce;
    idrq.fetch_callback = idfetch_update_cb;
    idrq.db = db;
    idrq.compacting = 0;

    new_id_root = modify_btree(&idrq, db->header.by_id_root, &errcode);
    error_pass(errcode);

    while (fetcharg.valpos < numdocs) {
        seqacts[fetcharg.actpos].type = ACTION_INSERT;
        seqacts[fetcharg.actpos].value.data = &seqvals[fetcharg.valpos];
        seqacts[fetcharg.actpos].key = &seqs[fetcharg.valpos];
        fetcharg.valpos++;
        fetcharg.actpos++;
    }

    //printf("Total seq actions: %d\n", fetcharg.actpos);
    qsort(seqacts, fetcharg.actpos, sizeof(couchfile_modify_action),
          seq_action_compare);

    seqrq.cmp.compare = seq_cmp;
    seqrq.cmp.arg = &tmpsb;
    seqrq.actions = seqacts;
    seqrq.num_actions = fetcharg.actpos;
    seqrq.reduce = by_seq_reduce;
    seqrq.rereduce = by_seq_rereduce;
    seqrq.db = db;
    seqrq.compacting = 0;

    new_seq_root = modify_btree(&seqrq, db->header.by_seq_root, &errcode);
    if (errcode != COUCHSTORE_SUCCESS) {
        fatbuf_free(actbuf);
        return errcode;
    }

    if (db->header.by_id_root != new_id_root) {
        free(db->header.by_id_root);
        db->header.by_id_root = new_id_root;
    }

    if (db->header.by_seq_root != new_seq_root) {
        free(db->header.by_seq_root);
        db->header.by_seq_root = new_seq_root;
    }

cleanup:
    free(sorted_ids);
    fatbuf_free(actbuf);
    return errcode;
}

static couchstore_error_t add_doc_to_update_list(Db *db,
                                                 const Doc *doc,
                                                 const DocInfo *info,
                                                 fatbuf *fb,
                                                 sized_buf *seqterm,
                                                 sized_buf *idterm,
                                                 sized_buf *seqval,
                                                 sized_buf *idval,
                                                 uint64_t seq,
                                                 couchstore_save_options options)
{
    couchstore_error_t errcode = COUCHSTORE_SUCCESS;
    DocInfo updated = *info;
    updated.db_seq = seq;

    seqterm->buf = (char *) fatbuf_get(fb, 6);
    seqterm->size = 6;
    error_unless(seqterm->buf, COUCHSTORE_ERROR_ALLOC_FAIL);
    *(raw_48*)seqterm->buf = encode_raw48(seq);

    if (doc) {
        size_t disk_size;

        // Don't compress a doc unless the meta flag is set
        if (!(info->content_meta & COUCH_DOC_IS_COMPRESSED)) {
            options &= ~COMPRESS_DOC_BODIES;
        }
        errcode = write_doc(db, doc, &updated.bp, &disk_size, options);

        if (errcode != COUCHSTORE_SUCCESS) {
            return errcode;
        }
        updated.size = disk_size;
    } else {
        updated.deleted = 1;
        updated.bp = 0;
        updated.size = 0;
    }

    *idterm = updated.id;

    seqval->buf = (char *) fatbuf_get(fb, (44 + updated.id.size + updated.rev_meta.size));
    error_unless(seqval->buf, COUCHSTORE_ERROR_ALLOC_FAIL);
    seqval->size = assemble_seq_index_value(&updated, seqval->buf);

    idval->buf = (char *) fatbuf_get(fb, (44 + 10 + updated.rev_meta.size));
    error_unless(idval->buf, COUCHSTORE_ERROR_ALLOC_FAIL);
    idval->size = assemble_id_index_value(&updated, idval->buf);

    //We use 37 + id.size + 2 * rev_meta.size bytes
cleanup:
    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_save_documents(Db *db,
                                             Doc* const docs[],
                                             DocInfo *infos[],
                                             unsigned numdocs,
                                             couchstore_save_options options)
{
    couchstore_error_t errcode = COUCHSTORE_SUCCESS;
    unsigned ii;
    sized_buf *seqklist, *idklist, *seqvlist, *idvlist;
    size_t term_meta_size = 0;
    const Doc *curdoc;
    uint64_t seq = db->header.update_seq;

    fatbuf *fb;
    
    for (ii = 0; ii < numdocs; ii++) {
        // Get additional size for terms to be inserted into indexes
        // IMPORTANT: This must match the sizes of the fatbuf_get calls in add_doc_to_update_list!
        term_meta_size += 6
                        + 44 + infos[ii]->id.size + infos[ii]->rev_meta.size
                        + 44 + 10 + infos[ii]->rev_meta.size;
    }

    fb = fatbuf_alloc(term_meta_size +
                      numdocs * (sizeof(sized_buf) * 4)); //seq/id key and value lists

    if (fb == NULL) {
        return COUCHSTORE_ERROR_ALLOC_FAIL;
    }


    seqklist = fatbuf_get(fb, numdocs * sizeof(sized_buf));
    idklist = fatbuf_get(fb, numdocs * sizeof(sized_buf));
    seqvlist = fatbuf_get(fb, numdocs * sizeof(sized_buf));
    idvlist = fatbuf_get(fb, numdocs * sizeof(sized_buf));

    for (ii = 0; ii < numdocs; ii++) {
        seq++;
        if (docs) {
            curdoc = docs[ii];
        } else {
            curdoc = NULL;
        }

        errcode = add_doc_to_update_list(db, curdoc, infos[ii], fb,
                                         &seqklist[ii], &idklist[ii],
                                         &seqvlist[ii], &idvlist[ii],
                                         seq, options);
        if (errcode != COUCHSTORE_SUCCESS) {
            break;
        }
    }

    if (errcode == COUCHSTORE_SUCCESS) {
        errcode = update_indexes(db, seqklist, seqvlist,
                                 idklist, idvlist, numdocs);
    }

    fatbuf_free(fb);
    if (errcode == COUCHSTORE_SUCCESS) {
        // Fill in the assigned sequence numbers for caller's later use:
        seq = db->header.update_seq;
        for (ii = 0; ii < numdocs; ii++) {
            infos[ii]->db_seq = ++seq;
        }
        db->header.update_seq = seq;
    }

    return errcode;
}

LIBCOUCHSTORE_API
couchstore_error_t couchstore_save_document(Db *db, const Doc *doc,
                                            DocInfo *info, couchstore_save_options options)
{
    return couchstore_save_documents(db, (Doc**)&doc, (DocInfo**)&info, 1, options);
}
