/* Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Memory handler for a shared memory divided in slot.
 * This one uses shared memory.
 *
 * Shared memory is cleaned-up for each restart, graceful or
 * otherwise.
 */

#include  "ap_slotmem.h"

#include "httpd.h"
#include "http_main.h"
#include "http_core.h"

#define AP_SLOTMEM_IS_PREGRAB(t)    (t->desc->type & AP_SLOTMEM_TYPE_PREGRAB)
#define AP_SLOTMEM_IS_PERSIST(t)    (t->desc->type & AP_SLOTMEM_TYPE_PERSIST)
#define AP_SLOTMEM_IS_CLEARINUSE(t) (t->desc->type & AP_SLOTMEM_TYPE_CLEARINUSE)

/* The description of the slots to reuse the slotmem */
typedef struct {
    apr_size_t size;             /* size of each memory slot */
    unsigned int num;            /* number of mem slots */
    ap_slotmem_type_t type;      /* type-specific flags */
} sharedslotdesc_t;

#define AP_SLOTMEM_OFFSET (APR_ALIGN_DEFAULT(sizeof(sharedslotdesc_t)))
#define AP_UNSIGNEDINT_OFFSET (APR_ALIGN_DEFAULT(sizeof(unsigned int)))

struct ap_slotmem_instance_t {
    char                 *name;       /* file based SHM path/name */
    char                 *pname;      /* persisted file path/name */
    int                  fbased;      /* filebased? */
    void                 *shm;        /* ptr to memory segment (apr_shm_t *) */
    void                 *base;       /* data set start */
    apr_pool_t           *pool;       /* per segment pool (generation cleared) */
    char                 *inuse;      /* in-use flag table*/
    unsigned int         *num_free;   /* slot free count for this instance */
    void                 *persist;    /* persist dataset start */
    const sharedslotdesc_t *desc;     /* per slot desc */
    struct ap_slotmem_instance_t  *next;       /* location of next allocated segment */
};

/*
 * Memory layout:
 *     sharedslotdesc_t | num_free | slots | isuse array |
 *                      ^          ^
 *                      |          . base
 *                      . persist (also num_free)
 */

/* global pool and list of slotmem we are handling */
static struct ap_slotmem_instance_t *globallistmem = NULL,
                                   **retained_globallistmem = NULL;
static apr_pool_t *gpool = NULL;

#define DEFAULT_SLOTMEM_PREFIX "slotmem-shm-"
#define DEFAULT_SLOTMEM_SUFFIX ".shm"
#define DEFAULT_SLOTMEM_PERSIST_SUFFIX ".persist"

/* Unixes (and Netware) have the unlink() semantic, which allows to
 * apr_file_remove() a file still in use (opened elsewhere), the inode
 * remains until the last fd is closed, whereas any file created with
 * the same name/path will use a new inode.
 *
 * On windows and OS/2 ("\SHAREMEM\..." tree), apr_file_remove() marks
 * the files for deletion until the last HANDLE is closed, meanwhile the
 * same file/path can't be opened/recreated.
 * Thus on graceful restart (the only restart mode with mpm_winnt), the
 * old file may still exist until all the children stop, while we ought
 * to create a new one for our new clear SHM.  Therefore, we would only
 * be able to reuse (attach) the old SHM, preventing some changes to
 * the config file, like the number of balancers/members, since the
 * size checks (to fit the new config) would fail.  Let's avoid this by
 * including the generation number in the SHM filename (obviously not
 * the persisted name!)
 */
#ifndef SLOTMEM_UNLINK_SEMANTIC
#if defined(WIN32) || defined(OS2)
#define SLOTMEM_UNLINK_SEMANTIC 0
#else
#define SLOTMEM_UNLINK_SEMANTIC 1
#endif
#endif

/*
 * Persist the slotmem in a file
 * slotmem name and file name.
 * none      : no persistent data
 * rel_name  : $server_root/rel_name
 * /abs_name : $abs_name
 *
 */
static int slotmem_filenames(apr_pool_t *pool,
                             const char *slotname,
                             const char **filename,
                             const char **persistname)
{
    const char *fname = NULL, *pname = NULL;

    if (slotname && *slotname && strcasecmp(slotname, "none") != 0) {
        if (slotname[0] != '/') {
            fname = apr_pstrcat(pool, DEFAULT_SLOTMEM_PREFIX,
                                slotname, DEFAULT_SLOTMEM_SUFFIX,
                                NULL);
            fname = ap_runtime_dir_relative(pool, fname);
        }
        else {
            /* Don't mangle the file name if given an absolute path, it's
             * up to the caller to provide a unique name when necessary.
             */
            fname = slotname;
        }

        if (persistname) {
            pname = apr_pstrcat(pool, fname,
                                DEFAULT_SLOTMEM_PERSIST_SUFFIX,
                                NULL);
        }
    }

    *filename = fname;
    if (persistname) {
        *persistname = pname;
    }
    return (fname != NULL);
}

static void slotmem_clearinuse(ap_slotmem_instance_t *slot)
{
    unsigned int i;
    char *inuse;
    
    if (!slot) {
        return;
    }
    
    inuse = slot->inuse;
    
    for (i = 0; i < slot->desc->num; i++, inuse++) {
        if (*inuse) {
            *inuse = 0;
            (*slot->num_free)++;
        }
    }
}

static void store_slotmem(ap_slotmem_instance_t *slotmem)
{
    apr_file_t *fp;
    apr_status_t rv;
    apr_size_t nbytes;
    unsigned char digest[APR_MD5_DIGESTSIZE];
    const char *storename = slotmem->pname;

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf, APLOGNO(02334)
                 "storing %s", storename);

    if (storename) {
        rv = apr_file_open(&fp, storename, APR_CREATE | APR_READ | APR_WRITE,
                           APR_OS_DEFAULT, slotmem->pool);
        if (APR_STATUS_IS_EEXIST(rv)) {
            apr_file_remove(storename, slotmem->pool);
            rv = apr_file_open(&fp, storename, APR_CREATE | APR_READ | APR_WRITE,
                               APR_OS_DEFAULT, slotmem->pool);
        }
        if (rv != APR_SUCCESS) {
            return;
        }
        if (AP_SLOTMEM_IS_CLEARINUSE(slotmem)) {
            slotmem_clearinuse(slotmem);
        }
        nbytes = (slotmem->desc->size * slotmem->desc->num) +
                 (slotmem->desc->num * sizeof(char)) + AP_UNSIGNEDINT_OFFSET;
        apr_md5(digest, slotmem->persist, nbytes);
        rv = apr_file_write_full(fp, slotmem->persist, nbytes, NULL);
        if (rv == APR_SUCCESS) {
            rv = apr_file_write_full(fp, digest, APR_MD5_DIGESTSIZE, NULL);
        }
        if (rv == APR_SUCCESS) {
            rv = apr_file_write_full(fp, slotmem->desc, AP_SLOTMEM_OFFSET,
                                     NULL);
        }
        apr_file_close(fp);
        if (rv != APR_SUCCESS) {
            apr_file_remove(storename, slotmem->pool);
        }
    }
}

static apr_status_t restore_slotmem(sharedslotdesc_t *desc,
                                    const char *storename, apr_size_t size,
                                    apr_pool_t *pool)
{
    apr_file_t *fp;
    apr_status_t rv = APR_ENOTIMPL;
    void *ptr = (char *)desc + AP_SLOTMEM_OFFSET;
    apr_size_t dsize = size - AP_SLOTMEM_OFFSET;
    apr_size_t nbytes = dsize;
    unsigned char digest[APR_MD5_DIGESTSIZE];
    unsigned char digest2[APR_MD5_DIGESTSIZE];
    char desc_buf[AP_SLOTMEM_OFFSET];

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf, APLOGNO(02335)
                 "restoring %s", storename);

    if (storename) {
        rv = apr_file_open(&fp, storename, APR_READ | APR_WRITE, APR_OS_DEFAULT,
                           pool);
        if (rv == APR_SUCCESS) {
            rv = apr_file_read(fp, ptr, &nbytes);
            if ((rv == APR_SUCCESS || rv == APR_EOF) && nbytes == dsize) {
                rv = APR_SUCCESS;   /* for successful return @ EOF */
                /*
                 * if at EOF, don't bother checking md5
                 *  - backwards compatibility
                 *  */
                if (apr_file_eof(fp) != APR_EOF) {
                    apr_size_t ds = APR_MD5_DIGESTSIZE;
                    rv = apr_file_read(fp, digest, &ds);
                    if ((rv == APR_SUCCESS || rv == APR_EOF)
                            && ds == APR_MD5_DIGESTSIZE) {
                        apr_md5(digest2, ptr, nbytes);
                        if (memcmp(digest, digest2, APR_MD5_DIGESTSIZE)) {
                            rv = APR_EMISMATCH;
                        }
                        /*
                         * if at EOF, don't bother checking desc
                         *  - backwards compatibility
                         *  */
                        else if (apr_file_eof(fp) != APR_EOF) {
                            nbytes = sizeof(desc_buf);
                            rv = apr_file_read(fp, desc_buf, &nbytes);
                            if ((rv == APR_SUCCESS || rv == APR_EOF)
                                    && nbytes == sizeof(desc_buf)) {
                                if (memcmp(desc, desc_buf, nbytes)) {
                                    rv = APR_EMISMATCH;
                                }
                                else {
                                    rv = APR_SUCCESS;
                                }
                            }
                            else if (rv == APR_SUCCESS || rv == APR_EOF) {
                                rv = APR_INCOMPLETE;
                            }
                        }
                        else {
                            rv = APR_EOF;
                        }
                    }
                    else if (rv == APR_SUCCESS || rv == APR_EOF) {
                        rv = APR_INCOMPLETE;
                    }
                }
                else {
                    rv = APR_EOF;
                }
                if (rv == APR_EMISMATCH) {
                    ap_log_error(APLOG_MARK, APLOG_ERR, 0, ap_server_conf, APLOGNO(02551)
                                 "persisted slotmem md5/desc mismatch");
                }
                else if (rv == APR_EOF) {
                    ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, ap_server_conf, APLOGNO(02552)
                                 "persisted slotmem at EOF... bypassing md5/desc match check "
                                 "(old persist file?)");
                    rv = APR_SUCCESS;
                }
            }
            else if (rv == APR_SUCCESS || rv == APR_EOF) {
                rv = APR_INCOMPLETE;
            }
            if (rv == APR_INCOMPLETE) {
                ap_log_error(APLOG_MARK, APLOG_ERR, 0, ap_server_conf, APLOGNO(02553)
                             "persisted slotmem read had unexpected size");
            }
            apr_file_close(fp);
        }
    }
    return rv;
}

static apr_status_t cleanup_slotmem(void *is_startup)
{
    int is_exiting = (ap_state_query(AP_SQ_MAIN_STATE) == AP_SQ_MS_EXITING);
    ap_slotmem_instance_t *mem;

    /* When in startup/pre-config's cleanup, the retained data and global pool
     * are not used yet, but the SHMs contents were untouched hence they don't
     * need to be persisted, simply unlink them.
     * Otherwise when restarting or stopping we want to flush persisted data,
     * and in the stopping/exiting case we also want to unlink the SHMs.
     */
    for (mem = globallistmem; mem; mem = mem->next) {
        int unlink;
        if (is_startup) {
            unlink = mem->fbased;
        }
        else {
            if (AP_SLOTMEM_IS_PERSIST(mem)) {
                store_slotmem(mem);
            }
            unlink = is_exiting;
        }
        if (unlink) {
            /* Some systems may require the descriptor to be closed before
             * unlink, thus call destroy() first (this won't free mem->shm
             * so it's safe to call delete() afterward).
             */
            apr_shm_destroy(mem->shm);
            apr_shm_delete(mem->shm);
        }
    }

    if (is_exiting) {
        *retained_globallistmem = NULL;
    }
    else if (!is_startup) {
        *retained_globallistmem = globallistmem;
    }
    globallistmem = NULL;

    return APR_SUCCESS;
}

static apr_status_t slotmem_doall(ap_slotmem_instance_t *mem,
                                  ap_slotmem_callback_fn_t *func,
                                  void *data, apr_pool_t *pool)
{
    unsigned int i;
    char *ptr;
    char *inuse;
    apr_status_t retval = APR_SUCCESS;

    if (!mem) {
        return APR_ENOSHMAVAIL;
    }

    ptr = (char *)mem->base;
    inuse = mem->inuse;
    for (i = 0; i < mem->desc->num; i++, inuse++) {
        if (!AP_SLOTMEM_IS_PREGRAB(mem) ||
           (AP_SLOTMEM_IS_PREGRAB(mem) && *inuse)) {
            retval = func((void *) ptr, data, pool);
            if (retval != APR_SUCCESS)
                break;
        }
        ptr += mem->desc->size;
    }
    return retval;
}

static apr_status_t slotmem_create(ap_slotmem_instance_t **new,
                                   const char *name, apr_size_t item_size,
                                   unsigned int item_num,
                                   ap_slotmem_type_t type, apr_pool_t *pool)
{
    int fbased = 1;
    int restored = 0;
    char *ptr;
    sharedslotdesc_t *desc;
    ap_slotmem_instance_t *res;
    ap_slotmem_instance_t *next = globallistmem;
    const char *fname, *pname = NULL;
    apr_shm_t *shm;
    apr_size_t basesize = (item_size * item_num);
    apr_size_t size = AP_SLOTMEM_OFFSET + AP_UNSIGNEDINT_OFFSET +
                      (item_num * sizeof(char)) + basesize;
    int persist = (type & AP_SLOTMEM_TYPE_PERSIST) != 0;
    apr_status_t rv;
    apr_pool_t *p;

    if (slotmem_filenames(pool, name, &fname, persist ? &pname : NULL)) {
        /* first try to attach to existing slotmem */
        if (next) {
            for (;;) {
                if (strcmp(next->name, fname) == 0) {
                    /* we already have it */
                    *new = next;
                    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf, APLOGNO(02603)
                                 "create found %s in global list", fname);
                    return APR_SUCCESS;
                }
                if (!next->next) {
                     break;
                }
                next = next->next;
            }
        }
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf, APLOGNO(02602)
                     "create didn't find %s in global list", fname);
    }
    else {
        fbased = 0;
        fname = "none";
    }

    /* first try to attach to existing shared memory */
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf, APLOGNO(02300)
                 "create %s: %"APR_SIZE_T_FMT"/%u", fname, item_size,
                 item_num);


    {
        if (fbased) {
            apr_shm_remove(fname, pool);
            rv = apr_shm_create(&shm, size, fname, gpool);
        }
        else {
            rv = apr_shm_create(&shm, size, NULL, pool);
        }
        ap_log_error(APLOG_MARK, rv == APR_SUCCESS ? APLOG_DEBUG : APLOG_ERR,
                     rv, ap_server_conf, APLOGNO(02611)
                     "create: apr_shm_create(%s) %s",
                     fname ? fname : "",
                     rv == APR_SUCCESS ? "succeeded" : "failed");
        if (rv != APR_SUCCESS) {
            return rv;
        }

        desc = (sharedslotdesc_t *)apr_shm_baseaddr_get(shm);
        memset(desc, 0, size);
        desc->size = item_size;
        desc->num = item_num;
        desc->type = type;

        /*
         * TODO: Error check the below... What error makes
         * sense if the restore fails? Any?
         * For now, we continue with a fresh new slotmem,
         * but NOTICE in the log.
         */
        if (persist) {
            rv = restore_slotmem(desc, pname, size, pool);
            if (rv == APR_SUCCESS) {
                restored = 1;
            }
            else {
                /* just in case, re-zero */
                ap_log_error(APLOG_MARK, APLOG_NOTICE, 0, ap_server_conf,
                             APLOGNO(02554) "could not restore %s", fname);
                memset((char *)desc + AP_SLOTMEM_OFFSET, 0,
                       size - AP_SLOTMEM_OFFSET);
            }
        }
    }

    p = fbased ? gpool : pool;
    ptr = (char *)desc + AP_SLOTMEM_OFFSET;

    /* For the chained slotmem stuff */
    res = apr_pcalloc(p, sizeof(ap_slotmem_instance_t));
    res->name = apr_pstrdup(p, fname);
    res->pname = apr_pstrdup(p, pname);
    res->fbased = fbased;
    res->shm = shm;
    res->persist = (void *)ptr;
    res->num_free = (unsigned int *)ptr;
    ptr += AP_UNSIGNEDINT_OFFSET;
    if (!restored) {
        *res->num_free = item_num;
    }
    res->base = (void *)ptr;
    res->desc = desc;
    res->pool = pool;
    res->next = NULL;
    res->inuse = ptr + basesize;
    if (fbased) {
        if (globallistmem == NULL) {
            globallistmem = res;
        }
        else {
            next->next = res;
        }
    }

    *new = res;
    return APR_SUCCESS;
}

static apr_status_t slotmem_attach(ap_slotmem_instance_t **new,
                                   const char *name, apr_size_t *item_size,
                                   unsigned int *item_num, apr_pool_t *pool)
{
/*    void *slotmem = NULL; */
    char *ptr;
    ap_slotmem_instance_t *res;
    ap_slotmem_instance_t *next = globallistmem;
    sharedslotdesc_t *desc;
    const char *fname;
    apr_shm_t *shm;
    apr_status_t rv;

    if (!slotmem_filenames(pool, name, &fname, NULL)) {
        return APR_ENOSHMAVAIL;
    }

    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf, APLOGNO(02301)
                 "attach looking for %s", fname);

    /* first try to attach to existing slotmem */
    if (next) {
        for (;;) {
            if (strcmp(next->name, fname) == 0) {
                /* we already have it */
                *new = next;
                *item_size = next->desc->size;
                *item_num = next->desc->num;
                ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf,
                             APLOGNO(02302)
                             "attach found %s: %"APR_SIZE_T_FMT"/%u", fname,
                             *item_size, *item_num);
                return APR_SUCCESS;
            }
            if (!next->next) {
                 break;
            }
            next = next->next;
        }
    }

    /* next try to attach to existing shared memory */
    rv = apr_shm_attach(&shm, fname, gpool);
    if (rv != APR_SUCCESS) {
        return rv;
    }

    /* Read the description of the slotmem */
    desc = (sharedslotdesc_t *)apr_shm_baseaddr_get(shm);
    ptr = (char *)desc + AP_SLOTMEM_OFFSET;

    /* For the chained slotmem stuff */
    res = apr_pcalloc(gpool, sizeof(ap_slotmem_instance_t));
    res->name = apr_pstrdup(gpool, fname);
    res->fbased = 1;
    res->shm = shm;
    res->persist = (void *)ptr;
    res->num_free = (unsigned int *)ptr;
    ptr += AP_UNSIGNEDINT_OFFSET;
    res->base = (void *)ptr;
    res->desc = desc;
    res->pool = pool;
    res->inuse = ptr + (desc->size * desc->num);
    res->next = NULL;
    if (globallistmem == NULL) {
        globallistmem = res;
    }
    else {
        next->next = res;
    }

    *new = res;
    *item_size = desc->size;
    *item_num = desc->num;
    ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf,
                 APLOGNO(02303)
                 "attach found %s: %"APR_SIZE_T_FMT"/%u", fname,
                 *item_size, *item_num);
    return APR_SUCCESS;
}

static apr_status_t slotmem_dptr(ap_slotmem_instance_t *slot,
                                 unsigned int id, void **mem)
{
    char *ptr;

    if (!slot) {
        return APR_ENOSHMAVAIL;
    }
    if (id >= slot->desc->num) {
        return APR_EINVAL;
    }

    ptr = (char *)slot->base + slot->desc->size * id;
    if (!ptr) {
        return APR_ENOSHMAVAIL;
    }
    *mem = (void *)ptr;
    return APR_SUCCESS;
}

static apr_status_t slotmem_get(ap_slotmem_instance_t *slot, unsigned int id,
                                unsigned char *dest, apr_size_t dest_len)
{
    void *ptr;
    char *inuse;
    apr_status_t ret;

    if (!slot) {
        return APR_ENOSHMAVAIL;
    }

    inuse = slot->inuse + id;
    if (id >= slot->desc->num) {
        return APR_EINVAL;
    }
    if (AP_SLOTMEM_IS_PREGRAB(slot) && !*inuse) {
        return APR_NOTFOUND;
    }
    ret = slotmem_dptr(slot, id, &ptr);
    if (ret != APR_SUCCESS) {
        return ret;
    }
    *inuse = 1;
    memcpy(dest, ptr, dest_len); /* bounds check? */
    return APR_SUCCESS;
}

static apr_status_t slotmem_put(ap_slotmem_instance_t *slot, unsigned int id,
                                unsigned char *src, apr_size_t src_len)
{
    void *ptr;
    char *inuse;
    apr_status_t ret;

    if (!slot) {
        return APR_ENOSHMAVAIL;
    }

    inuse = slot->inuse + id;
    if (id >= slot->desc->num) {
        return APR_EINVAL;
    }
    if (AP_SLOTMEM_IS_PREGRAB(slot) && !*inuse) {
        return APR_NOTFOUND;
    }
    ret = slotmem_dptr(slot, id, &ptr);
    if (ret != APR_SUCCESS) {
        return ret;
    }
    *inuse=1;
    memcpy(ptr, src, src_len); /* bounds check? */
    return APR_SUCCESS;
}

static unsigned int slotmem_num_slots(ap_slotmem_instance_t *slot)
{
    return slot->desc->num;
}

static unsigned int slotmem_num_free_slots(ap_slotmem_instance_t *slot)
{
    if (AP_SLOTMEM_IS_PREGRAB(slot))
        return *slot->num_free;
    else {
        unsigned int i, counter=0;
        char *inuse = slot->inuse;
        for (i=0; i<slot->desc->num; i++, inuse++) {
            if (!*inuse)
                counter++;
        }
        return counter;
    }
}

static apr_size_t slotmem_slot_size(ap_slotmem_instance_t *slot)
{
    return slot->desc->size;
}

static apr_status_t slotmem_grab(ap_slotmem_instance_t *slot, unsigned int *id)
{
    unsigned int i;
    char *inuse;

    if (!slot) {
        return APR_ENOSHMAVAIL;
    }

    inuse = slot->inuse;

    for (i = 0; i < slot->desc->num; i++, inuse++) {
        if (!*inuse) {
            break;
        }
    }
    if (i >= slot->desc->num) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf, APLOGNO(02293)
                     "slotmem(%s) grab failed. Num %u/num_free %u",
                     slot->name, slotmem_num_slots(slot),
                     slotmem_num_free_slots(slot));
        return APR_EINVAL;
    }
    *inuse = 1;
    *id = i;
    (*slot->num_free)--;
    return APR_SUCCESS;
}

static apr_status_t slotmem_fgrab(ap_slotmem_instance_t *slot, unsigned int id)
{
    char *inuse;
    
    if (!slot) {
        return APR_ENOSHMAVAIL;
    }

    if (id >= slot->desc->num) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf, APLOGNO(02397)
                     "slotmem(%s) fgrab failed. Num %u/num_free %u",
                     slot->name, slotmem_num_slots(slot),
                     slotmem_num_free_slots(slot));
        return APR_EINVAL;
    }
    inuse = slot->inuse + id;

    if (!*inuse) {
        *inuse = 1;
        (*slot->num_free)--;
    }
    return APR_SUCCESS;
}

static apr_status_t slotmem_release(ap_slotmem_instance_t *slot,
                                    unsigned int id)
{
    char *inuse;

    if (!slot) {
        return APR_ENOSHMAVAIL;
    }

    inuse = slot->inuse;

    if (id >= slot->desc->num || !inuse[id] ) {
        ap_log_error(APLOG_MARK, APLOG_DEBUG, 0, ap_server_conf, APLOGNO(02294)
                     "slotmem(%s) release failed. Num %u/inuse[%u] %d",
                     slot->name, slotmem_num_slots(slot),
                     id, (int)inuse[id]);
        if (id >= slot->desc->num) {
            return APR_EINVAL;
        } else {
            return APR_NOTFOUND;
        }
    }
    inuse[id] = 0;
    (*slot->num_free)++;
    return APR_SUCCESS;
}

static const ap_slotmem_provider_t storage = {
    "sharedmem",
    &slotmem_doall,
    &slotmem_create,
    &slotmem_attach,
    &slotmem_dptr,
    &slotmem_get,
    &slotmem_put,
    &slotmem_num_slots,
    &slotmem_num_free_slots,
    &slotmem_slot_size,
    &slotmem_grab,
    &slotmem_release,
    &slotmem_fgrab
};

/* make the storage usable from outside */
static const ap_slotmem_provider_t *slotmem_shm_getstorage(void)
{
    return (&storage);
}

/* Initialize or reuse the retained slotmems list, and register the
 * cleanup to make sure the persisted SHMs are stored and the retained
 * data are up to date on next restart/stop.
 */
static int pre_config(apr_pool_t *pconf, apr_pool_t *plog,
                      apr_pool_t *ptemp)
{
    void *is_startup = NULL;
    const char *retained_key = "mod_slotmem_shm";

    retained_globallistmem = ap_retained_data_get(retained_key);
    if (!retained_globallistmem) {
        retained_globallistmem =
            ap_retained_data_create(retained_key,
                                    sizeof *retained_globallistmem);
    }
    globallistmem = *retained_globallistmem;

    if (ap_state_query(AP_SQ_MAIN_STATE) != AP_SQ_MS_CREATE_PRE_CONFIG) {
        gpool = ap_pglobal;
    }
    else {
        is_startup = (void *)1;
        gpool = pconf;
    }

    apr_pool_cleanup_register(pconf, is_startup, cleanup_slotmem,
                              apr_pool_cleanup_null);

    return OK;
}

static void ap_slotmem_shm_register_hook(apr_pool_t *p)
{
    const ap_slotmem_provider_t *storage = slotmem_shm_getstorage();
    ap_register_provider(p, AP_SLOTMEM_PROVIDER_GROUP, "shm",
                         AP_SLOTMEM_PROVIDER_VERSION, storage);
    ap_hook_pre_config(pre_config, NULL, NULL, APR_HOOK_MIDDLE);
}

AP_DECLARE_MODULE(slotmem_shm) = {
    STANDARD20_MODULE_STUFF,
    NULL,                       /* create per-directory config structure */
    NULL,                       /* merge per-directory config structures */
    NULL,                       /* create per-server config structure */
    NULL,                       /* merge per-server config structures */
    NULL,                       /* command apr_table_t */
    ap_slotmem_shm_register_hook  /* register hooks */
};
