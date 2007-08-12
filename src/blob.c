/**
 * Copyright (C) 2005-2007 Christoph Rupp (chris@crupp.de).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or 
 * (at your option) any later version.
 *
 * See file COPYING.GPL2 and COPYING.GPL3 for License information.
 *
 */

#include <string.h>
#include <ham/hamsterdb.h>
#include "os.h"
#include "db.h"
#include "blob.h"
#include "error.h"
#include "freelist.h"
#include "mem.h"
#include "page.h"

#define SMALLEST_CHUNK_SIZE  (sizeof(ham_offset_t)+sizeof(blob_t)+1)

static ham_bool_t
my_blob_is_small(ham_db_t *db, ham_size_t size)
{
    return (size<(ham_size_t)(db_get_pagesize(db)/3));
}

static ham_status_t
my_write_chunks(ham_db_t *db, ham_page_t *page, 
        ham_offset_t addr, ham_u8_t **chunk_data, ham_size_t *chunk_size, 
        ham_size_t chunks)
{
    ham_size_t i;
    ham_status_t st;
    ham_offset_t pageid;
    ham_device_t *device=db_get_device(db);

    /*
     * for each chunk...
     */
    for (i=0; i<chunks; i++) {
        while (chunk_size[i]) {
            /*
             * get the page-ID from this chunk
             */
            pageid=(addr/db_get_pagesize(db))*db_get_pagesize(db);

            /*
             * is it the current page? if not, try to fetch the page from
             * the cache - but only read the page from disk, if the 
             * chunk is small
             */
            if (!(page && page_get_self(page)==pageid) || 
                    my_blob_is_small(db, chunk_size[i])) {
                page=db_fetch_page(db, pageid, 
                        my_blob_is_small(db, chunk_size[i]) 
                        ? 0 : DB_ONLY_FROM_CACHE);
                /* blob pages don't have a page header */
                if (page)
                    page_set_npers_flags(page, 
                        page_get_npers_flags(page)|PAGE_NPERS_NO_HEADER);
            }

            /*
             * if we have a page pointer: use it; otherwise write directly
             * to the device
             */
            if (page) {
                ham_size_t writestart=
                        (ham_size_t)(addr-page_get_self(page));
                ham_size_t writesize =
                        (ham_size_t)(db_get_pagesize(db)-writestart);
                if (writesize>chunk_size[i])
                    writesize=chunk_size[i];
                memcpy(&page_get_raw_payload(page)[writestart], chunk_data[i],
                            writesize);
                page_set_dirty(page, 1);
                addr+=writesize;
                chunk_data[i]+=writesize;
                chunk_size[i]-=writesize;
            }
            else {
                ham_size_t s=chunk_size[i]<db_get_pagesize(db) 
                        ? chunk_size[i] : db_get_pagesize(db);
                /* limit to the next page boundary */
                if (s>pageid+db_get_pagesize(db)-addr)
                    s=(ham_size_t)(pageid+db_get_pagesize(db)-addr);

                st=device->write(device, addr, chunk_data[i], s);
                if (st)
                    return (st);
                addr+=s;
                chunk_data[i]+=s;
                chunk_size[i]-=s;
            }
        }
    }

    return (0);
}

static ham_status_t
my_read_chunk(ham_db_t *db, ham_offset_t addr, 
        ham_u8_t *data, ham_size_t size)
{
    ham_status_t st;
    ham_page_t *page=0;
    ham_offset_t pageid;
    ham_device_t *device=db_get_device(db);

    while (size) {
        /*
         * get the page-ID from this chunk
         */
        pageid=(addr/db_get_pagesize(db))*db_get_pagesize(db);

        /*
         * is it the current page? if not, try to fetch the page from
         * the cache - but only read the page from disk, if the 
         * chunk is small
         */
        if (!(page && page_get_self(page)==pageid) || 
                my_blob_is_small(db, size)) {
            page=db_fetch_page(db, pageid, 
                    my_blob_is_small(db, size) ? 0 : DB_ONLY_FROM_CACHE);
            /* blob pages don't have a page header */
            if (page)
                page_set_npers_flags(page, 
                    page_get_npers_flags(page)|PAGE_NPERS_NO_HEADER);
            else
                if (db_get_error(db))
                    return (db_get_error(db));
        }

        /*
         * if we have a page pointer: use it; otherwise read directly
         * from the device
         */
        if (page) {
            ham_size_t readstart=
                    (ham_size_t)(addr-page_get_self(page));
            ham_size_t readsize =
                    (ham_size_t)(db_get_pagesize(db)-readstart);
            if (readsize>size)
                readsize=size;
            memcpy(data, &page_get_raw_payload(page)[readstart], readsize);
            addr+=readsize;
            data+=readsize;
            size-=readsize;
        }
        else {
            ham_size_t s=size<db_get_pagesize(db) 
                    ? size : db_get_pagesize(db);
            /* limit to the next page boundary */
            if (s>pageid+db_get_pagesize(db)-addr)
                s=(ham_size_t)(pageid+db_get_pagesize(db)-addr);

            st=device->read(device, addr, data, s);
            if (st) 
                return (st);
            addr+=s;
            data+=s;
            size-=s;
        }
    }

    return (0);
}

ham_status_t
blob_allocate(ham_db_t *db, ham_u8_t *data, ham_size_t size, 
        ham_u32_t flags, ham_offset_t next, ham_offset_t *blobid)
{
    ham_status_t st;
    ham_page_t *page=0;
    ham_offset_t addr;
    blob_t hdr;
    ham_u8_t *chunk_data[2];
    ham_size_t alloc_size, chunk_size[2];
    ham_device_t *device=db_get_device(db);
   
    *blobid=0;

    /*
     * in-memory-database: the blobid is actually a pointer to the memory
     * buffer, in which the blob (with the blob-header) is stored
     */
    if (db_get_rt_flags(db)&HAM_IN_MEMORY_DB) {
        blob_t *hdr;
        ham_u8_t *p=(ham_u8_t *)ham_mem_alloc(db, size+sizeof(blob_t));
        if (!p) {
            db_set_error(db, HAM_OUT_OF_MEMORY);
            return (HAM_OUT_OF_MEMORY);
        }
        memcpy(p+sizeof(blob_t), data, size);

        /* initialize the header */
        hdr=(blob_t *)p;
        memset(hdr, 0, sizeof(&hdr));
        blob_set_self(hdr, (ham_offset_t)p);
        blob_set_alloc_size(hdr, size+sizeof(blob_t));
        blob_set_real_size(hdr, size+sizeof(blob_t));
        blob_set_user_size(hdr, size);
        blob_set_next(hdr, next);

        *blobid=(ham_offset_t)p;
        return (0);
    }

    memset(&hdr, 0, sizeof(hdr));

    /*
     * blobs are CHUNKSIZE-allocated 
     */
    alloc_size=sizeof(blob_t)+size;
    if (alloc_size%DB_CHUNKSIZE!=0)
        alloc_size=((alloc_size/DB_CHUNKSIZE)*DB_CHUNKSIZE)+DB_CHUNKSIZE;

    /* 
     * check if we have space in the freelist 
     */
    addr=freel_alloc_area(db, alloc_size);
    if (!addr) {
        /*
         * if the blob is small, we load the page through the cache
         */
        if (my_blob_is_small(db, alloc_size)) {
            page=db_alloc_page(db, PAGE_TYPE_B_INDEX|PAGE_IGNORE_FREELIST, 0);
            if (!page)
                return (db_get_error(db));
            /* blob pages don't have a page header */
            page_set_npers_flags(page, 
                    page_get_npers_flags(page)|PAGE_NPERS_NO_HEADER);
            addr=page_get_self(page);
            /* move the remaining space to the freelist */
            (void)freel_mark_free(db, addr+alloc_size,
                    db_get_pagesize(db)-alloc_size);
            blob_set_alloc_size(&hdr, alloc_size);
        }
        /*
         * otherwise use direct IO to allocate the space
         */
        else {
            ham_size_t aligned=alloc_size;
            if (aligned%db_get_pagesize(db)) {
                aligned+=db_get_pagesize(db);
                aligned/=db_get_pagesize(db);
                aligned*=db_get_pagesize(db);
            }

            st=device->alloc(device, aligned, &addr);
            if (st) 
                return (st);

            /* if aligned!=size, and the remaining chunk is large enough:
             * move it to the freelist */
            if (aligned!=alloc_size) {
                ham_size_t diff=aligned-alloc_size;
                if (diff>SMALLEST_CHUNK_SIZE) {
                    (void)freel_mark_free(db, addr+alloc_size, diff);
                    blob_set_alloc_size(&hdr, aligned-diff);
                }
                else 
                    blob_set_alloc_size(&hdr, aligned);
            }
            else
                blob_set_alloc_size(&hdr, aligned);
        }
    }
    else
        blob_set_alloc_size(&hdr, alloc_size);

    blob_set_real_size(&hdr, sizeof(blob_t)+size);
    blob_set_user_size(&hdr, size);
    blob_set_self(&hdr, addr);
    blob_set_next(&hdr, next);

    /* 
     * write header and data 
     */
    chunk_data[0]=(ham_u8_t *)&hdr;
    chunk_size[0]=sizeof(hdr);
    chunk_data[1]=(ham_u8_t *)data;
    chunk_size[1]=size;

    st=my_write_chunks(db, page, addr, chunk_data, chunk_size, 2);
    if (st)
        return (st);

    *blobid=addr;

    return (0);
}

ham_status_t
blob_read(ham_db_t *db, ham_offset_t blobid, 
        ham_record_t *record, ham_u32_t flags)
{
    ham_status_t st;
    blob_t hdr;

    record->size=0;

    /*
     * in-memory-database: the blobid is actually a pointer to the memory
     * buffer, in which the blob is stored
     */
    if (db_get_rt_flags(db)&HAM_IN_MEMORY_DB) {
        blob_t *hdr=(blob_t *)blobid;
        ham_u8_t *data=((ham_u8_t *)blobid)+sizeof(blob_t);

        /* when the database is closing, the header is already deleted */
        if (!hdr)
            return (0);

        /* empty blob? */
        record->size=(ham_size_t)blob_get_user_size(hdr);
        if (!record->size) {
            record->data=0;
            return (0);
        }

        /* resize buffer, if necessary */
        if (!(record->flags & HAM_RECORD_USER_ALLOC)) {
            if (blob_get_user_size(hdr)>db_get_record_allocsize(db)) {
                void *newdata=ham_mem_alloc(db, 
                        (ham_u32_t)blob_get_user_size(hdr));
                if (!newdata) 
                    return (HAM_OUT_OF_MEMORY);
                if (db_get_record_allocdata(db))
                    ham_mem_free(db, db_get_record_allocdata(db));
                record->data=newdata;
                db_set_record_allocdata(db, newdata);
                db_set_record_allocsize(db,(ham_size_t)blob_get_user_size(hdr));
            }
            else
                record->data=db_get_record_allocdata(db);
        }

        /* and copy the data */
        memcpy(record->data, data, record->size);

        return (0);
    }

    ham_assert(blobid%DB_CHUNKSIZE==0, ("blobid is %llu", blobid));

    /*
     * first step: read the blob header 
     */
    st=my_read_chunk(db, blobid, (ham_u8_t *)&hdr, sizeof(hdr));
    if (st)
        return (st);

    ham_assert(blob_get_alloc_size(&hdr)%DB_CHUNKSIZE==0, (0));

    /*
     * sanity check
     */
    ham_assert(blob_get_self(&hdr)==blobid, 
            ("invalid blobid %llu != %llu", blob_get_self(&hdr), blobid));
    if (blob_get_self(&hdr)!=blobid)
        return (HAM_BLOB_NOT_FOUND);

    /* 
     * empty blob? 
     */
    record->size=(ham_size_t)blob_get_user_size(&hdr);
    if (!record->size) {
        record->data=0;
        return (0);
    }

    /*
     * second step: resize the blob buffer
     */
    if (!(record->flags & HAM_RECORD_USER_ALLOC)) {
        if (blob_get_real_size(&hdr)>db_get_record_allocsize(db)) {
            void *newdata=ham_mem_alloc(db, 
                    (ham_size_t)blob_get_real_size(&hdr));
            if (!newdata) 
                return (HAM_OUT_OF_MEMORY);
            if (db_get_record_allocdata(db))
                ham_mem_free(db, db_get_record_allocdata(db));
            record->data=newdata;
            db_set_record_allocdata(db, newdata);
            db_set_record_allocsize(db, (ham_size_t)blob_get_real_size(&hdr));
        }
        else
            record->data=db_get_record_allocdata(db);
    }

    /*
     * third step: read the blob data
     */
    st=my_read_chunk(db, blobid+sizeof(blob_t), record->data, 
            (ham_size_t)blob_get_user_size(&hdr));
    if (st)
        return (st);

    record->size=(ham_size_t)blob_get_user_size(&hdr);

    return (0);
}

ham_status_t
blob_replace(ham_db_t *db, ham_offset_t old_blobid, 
        ham_u8_t *data, ham_size_t size, ham_u32_t flags, 
        ham_offset_t *new_blobid)
{
    ham_status_t st;
    ham_size_t alloc_size;
    blob_t old_hdr, new_hdr;

    /*
     * inmemory-databases: free the old blob, 
     * allocate a new blob
     */
    if (db_get_rt_flags(db)&HAM_IN_MEMORY_DB) {
        st=blob_free(db, old_blobid, flags);
        if (st)
            return (st);
        return (blob_allocate(db, data, size, flags, 0, new_blobid));
    }

    ham_assert(old_blobid%DB_CHUNKSIZE==0, (0));

    /*
     * blobs are CHUNKSIZE-allocated 
     */
    alloc_size=sizeof(blob_t)+size;
    if (alloc_size%DB_CHUNKSIZE!=0)
        alloc_size=((alloc_size/DB_CHUNKSIZE)*DB_CHUNKSIZE)+DB_CHUNKSIZE;

    /*
     * first, read the blob header; if the new blob fits into the 
     * old blob, we overwrite the old blob (and add the remaining
     * space to the freelist, if there is any)
     */
    st=my_read_chunk(db, old_blobid, (ham_u8_t *)&old_hdr, 
            sizeof(old_hdr));
    if (st)
        return (st);

    ham_assert(blob_get_alloc_size(&old_hdr)%DB_CHUNKSIZE==0, (0));

    /*
     * sanity check
     */
    ham_assert(blob_get_self(&old_hdr)==old_blobid, 
            ("invalid blobid %llu != %llu", blob_get_self(&old_hdr), 
            old_blobid));
    if (blob_get_self(&old_hdr)!=old_blobid)
        return (HAM_BLOB_NOT_FOUND);

    /*
     * now compare the sizes
     */
    if (alloc_size<=blob_get_alloc_size(&old_hdr)) {
        ham_u8_t *chunk_data[2]={(ham_u8_t *)&new_hdr, data};
        ham_size_t chunk_size[2]={sizeof(new_hdr), size};

        /* 
         * setup the new blob header
         */
        blob_set_self(&new_hdr, blob_get_self(&old_hdr));
        blob_set_user_size(&new_hdr, size);
        blob_set_real_size(&new_hdr, size+sizeof(blob_t));
        if (blob_get_alloc_size(&old_hdr)-alloc_size>SMALLEST_CHUNK_SIZE)
            blob_set_alloc_size(&new_hdr, alloc_size);
        else
            blob_set_alloc_size(&new_hdr, blob_get_alloc_size(&old_hdr));

        st=my_write_chunks(db, 0, blob_get_self(&new_hdr),
                chunk_data, chunk_size, 2);
        if (st)
            return (st);

        /*
         * move remaining data to the freelist
         */
        if (blob_get_alloc_size(&old_hdr)!=blob_get_alloc_size(&new_hdr)) {
            (void)freel_mark_free(db, 
                  blob_get_self(&new_hdr)+blob_get_alloc_size(&new_hdr), 
                  (ham_size_t)(blob_get_alloc_size(&old_hdr)-
                    blob_get_alloc_size(&new_hdr)));
        }

        /*
         * the old rid is the new rid
         */
        *new_blobid=blob_get_self(&new_hdr);

        return (0);
    }
    else {
        (void)freel_mark_free(db, old_blobid, 
                (ham_size_t)blob_get_alloc_size(&old_hdr));

        return (blob_allocate(db, data, size, flags, 0, new_blobid));
    }
}

ham_status_t
blob_free(ham_db_t *db, ham_offset_t blobid, ham_u32_t flags)
{
    ham_status_t st;
    ham_offset_t newhead=0;

    do {
        st=blob_free_dupes(db, blobid, flags, &newhead);
        if (st)
            return (st);
        blobid=newhead;
    } while ((flags&BLOB_FREE_ALL_DUPES) && blobid);

    return (0);
}

ham_status_t
blob_free_dupes(ham_db_t *db, ham_offset_t blobid, ham_u32_t flags, 
        ham_offset_t *newhead)
{
    ham_status_t st;
    blob_t hdr;

    /*
     * in-memory-database: the blobid is actually a pointer to the memory
     * buffer, in which the blob is stored
     */
    if (db_get_rt_flags(db)&HAM_IN_MEMORY_DB) {
        blob_t *phdr=(ham_u8_t *)blobid;
        if (newhead)
            *newhead=blob_get_next(phdr);
        ham_mem_free(db, phdr);
        return (0);
    }

    ham_assert(blobid%DB_CHUNKSIZE==0, (0));

    /*
     * fetch the blob header 
     */
    st=my_read_chunk(db, blobid, (ham_u8_t *)&hdr, sizeof(hdr));
    if (st)
        return (st);

    ham_assert(blob_get_alloc_size(&hdr)%DB_CHUNKSIZE==0, (0));

    /*
     * sanity check
     */
    ham_assert(blob_get_self(&hdr)==blobid, 
            ("invalid blobid %llu != %llu", blob_get_self(&hdr), blobid);)
    if (blob_get_self(&hdr)!=blobid)
        return (HAM_BLOB_NOT_FOUND);

    if (newhead)
        *newhead=blob_get_next(&hdr);

    /*
     * move the blob to the freelist
     */
    (void)freel_mark_free(db, blobid, 
            (ham_size_t)blob_get_alloc_size(&hdr));

    return (0);
}
