/*
 * Copyright (c) 2013 Anchor Systems Pty Ltd
 * Copyright (c) 2013 Matt Palmer <matt@hezmatt.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "redis.h"
#include "nds.h"

#include <lmdb.h>

#include <stdio.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>

#define FREEZER_FILENAME_LEN 255

typedef struct {
	MDB_txn *txn;
	MDB_dbi dbi;
	redisDb *rdb;
} NDSDB;

/* Generate the name of the freezer we want, based on the database passed
 * in, and stuff the name into buf. */
static void freezer_filename(redisDb *db, char *buf) {
    snprintf(buf, FREEZER_FILENAME_LEN-1,
             "freezer_%i",
             db->id);
}

static int nds_init() {
    int rv = 0;
    
    if (server.mdb_env) {
        return REDIS_OK;
    }

    redisLog(REDIS_DEBUG, "initialising mdb_env");

    if ((rv = mdb_env_create(&server.mdb_env))) {
        redisLog(REDIS_WARNING, "mdb_env_create() failed: %s", mdb_strerror(rv));
        server.mdb_env = NULL;
        return REDIS_ERR;
    }
    
    if ((rv = mdb_env_set_mapsize(server.mdb_env, (size_t)1048576*1048576))) {  /* 1TB! */
        redisLog(REDIS_WARNING, "mdb_env_set_mapsize() failed: %s", mdb_strerror(rv));
        mdb_env_close(server.mdb_env);
        server.mdb_env = NULL;
        return REDIS_ERR;
    }
    
    if ((rv = mdb_env_set_maxdbs(server.mdb_env, server.dbnum))) {
        redisLog(REDIS_WARNING, "mdb_env_set_maxdbs() failed: %s", mdb_strerror(rv));
        mdb_env_close(server.mdb_env);
        server.mdb_env = NULL;
        return REDIS_ERR;
    }
    
    if ((rv = mdb_env_open(server.mdb_env, ".", 0, 0644))) {
        redisLog(REDIS_WARNING, "mdb_env_open() failed: %s", mdb_strerror(rv));
        mdb_env_close(server.mdb_env);
        server.mdb_env = NULL;
        return REDIS_ERR;
    }
    
    redisLog(REDIS_DEBUG, "mdb_env initialised");
    
    return REDIS_OK;
}

/* Close an NDS database. */
static void nds_close(NDSDB *ndsdb) {
    if (!ndsdb) {
        return;
    }

    if (ndsdb->txn) {
        mdb_txn_commit(ndsdb->txn);
    }
    
    if (ndsdb->dbi >= 0) {
        mdb_dbi_close(server.mdb_env, ndsdb->dbi);
    }
    
    zfree(ndsdb);
}

/* Open the freezer.  Pass in the redis DB to open the freezer for and
 * whether you want to open for read (writer == 0) or write (writer == 1). 
 * You'll get back an NDSDB * that can be handed around to other nds_*
 * functions, or NULL on failure.
 */
static NDSDB *nds_open(redisDb *db, int writer) {
    char db_name[FREEZER_FILENAME_LEN];
    NDSDB *ndsdb;
    int rv;
    
    if (nds_init() == REDIS_ERR) {
        return NULL;
    }
    
    ndsdb = zmalloc(sizeof(NDSDB));
    if (!ndsdb) {
        redisLog(REDIS_WARNING, "Failed to allocate an NDSDB: %s", strerror(errno));
        return NULL;
    }
    ndsdb->txn = NULL;
    ndsdb->rdb = db;
    
    freezer_filename(db, db_name);
    
    if ((rv = mdb_txn_begin(server.mdb_env, NULL, 0, &(ndsdb->txn)))) {
        redisLog(REDIS_WARNING, "Failed to open the freezer for DB %i: %s", db->id, mdb_strerror(rv));
        goto err_cleanup;
    }
    
    if ((rv = mdb_dbi_open(ndsdb->txn, db_name, MDB_CREATE, &(ndsdb->dbi)))) {
        redisLog(REDIS_WARNING, "Failed to open freezer DBi for DB %i: %s", db->id, mdb_strerror(rv));
        goto err_cleanup;
    }

    /* Epic win! */
    goto done;

err_cleanup:
    nds_close(ndsdb);
    ndsdb = NULL;

done:
    return ndsdb;    
}

/* Check whether a key exists in the NDS.  Give me a DB and a key, and
 * I'll give you a 1/0 to say whether it exists or not.  You'll get -1
 * if there was an error.
 */
static int nds_exists(NDSDB *db, sds key) {
    MDB_val k, v;
    int rv;
    
    k.mv_size = sdslen(key);
    k.mv_data = key;
    
    if (isDirtyKey(db->rdb, key)) {
        /* If the key's dirty but you're coming here, then it isn't in
         * memory, so it clearly mustn't exist.  */
        return 0;
    }
    
    rv = mdb_get(db->txn, db->dbi, &k, &v);
    
    if (rv == 0) {
        rv = 1;
    } else if (rv == MDB_NOTFOUND) {
        rv = 0;
    } else {
        redisLog(REDIS_WARNING, "mdb_get(%s) failed: %s", key, mdb_strerror(rv));
        rv = -1;
    }

    return rv;
}

/* Get a value out of the NDS.  Pass in the DB and key to get the value for,
 * and return an sds containing the value if found, or NULL on error or
 * key-not-found.  Will report errors via redisLog.  */
static sds nds_get(NDSDB *db, sds key) {
    MDB_val k, v;
    int rv;
    
    k.mv_size = sdslen(key);
    k.mv_data = key;
    
    if (isDirtyKey(db->rdb, key)) {
        /* A dirty key *must* be in memory if it still exists.  If you're
         * coming here, then the key *isn't* in memory, thus it does not
         * exist, and so I'm not going to go and get an out-of-date copy off
         * disk for you.
         */
        return NULL;
    }

    rv = mdb_get(db->txn, db->dbi, &k, &v);
    
    if (rv && rv != MDB_NOTFOUND) {
        redisLog(REDIS_WARNING, "mdb_get(%s) failed: %s", key, mdb_strerror(rv));
    }
    
    if (rv) {
        return NULL;
    }
    
    return sdsnewlen(v.mv_data, v.mv_size);
}

/* Set a value in the NDS.  Takes an NDSDB, a key, and a value, and makes
 * sure they get into the database (or you at least know what's going on via
 * the logs).  Returns REDIS_ERR on failure or REDIS_OK on success.  */
static int nds_set(NDSDB *db, sds key, sds val) {
    int rv = REDIS_OK;
    MDB_val k, v;
    
    k.mv_size = sdslen(key);
    k.mv_data = key;
    
    v.mv_size = sdslen(val);
    v.mv_data = val;
    
    rv = mdb_put(db->txn, db->dbi, &k, &v, 0);
    
    if (rv == MDB_TXN_FULL) {
        /* Odd... let's flush this transaction and try one more time */
        rv = mdb_txn_commit(db->txn);
        if (rv) {
            redisLog(REDIS_WARNING, "Failed to commit txn: %s", mdb_strerror(rv));
            mdb_dbi_close(server.mdb_env, db->dbi);
            mdb_txn_begin(server.mdb_env, NULL, 0, &(db->txn));
            mdb_dbi_open(db->txn, NULL, MDB_CREATE, &(db->dbi));
            return REDIS_ERR;
        } else {
	    return REDIS_OK;
	}
    } else if (rv) {
        redisLog(REDIS_WARNING, "mdb_put(%s) failed: %s", key, mdb_strerror(rv));
        return REDIS_ERR;
    } else {
        return REDIS_OK;
    }
}

/* Deletion time!  Take an NDSDB and a key, and make the key go away.  Tells
 * the user about problems via the logs, and returns 1 if a key was deleted,
 * 0 if no key was deleted, and -1 if an error occured. */
static int nds_del(NDSDB *db, sds key) {
    MDB_val k;
    int rv;
    
    k.mv_size = sdslen(key);
    k.mv_data = key;
    
    rv = mdb_del(db->txn, db->dbi, &k, NULL);
    
    if (rv == MDB_NOTFOUND) {
        return 0;
    } else if (rv) {
        redisLog(REDIS_WARNING, "nds_del(%s) failed: %s", key, mdb_strerror(rv));
        return -1;
    } else {
        return 1;
    }
}

static void nds_nuke(redisDb *db) {
    NDSDB *ndsdb = nds_open(db, 1);
    
    if (!ndsdb) {
        return;
    }
    
    mdb_drop(ndsdb->txn, ndsdb->dbi, 1);
    
    nds_close(ndsdb);
}

robj *getNDS(redisDb *db, robj *key) {
    sds val = NULL;
    rio payload;
    int type;
    robj *obj = NULL;
    NDSDB *ndsdb = nds_open(db, 0);
    
    redisLog(REDIS_DEBUG, "Looking up %s in NDS", (char *)key->ptr);

    if (!ndsdb) {
        return NULL;
    }
    
    val = nds_get(ndsdb, key->ptr);
    
    nds_close(ndsdb);
    
    if (val) {
        redisLog(REDIS_DEBUG, "Key %s was found in NDS", (char *)key->ptr);

        /* We got one!  Thaw and return */
        
        /* Is the data valid? */
        if (verifyDumpPayload((unsigned char *)val, (size_t)sdslen(val)) == REDIS_ERR) {
            redisLog(REDIS_WARNING, "Invalid payload for key %s; ignoring", (char *)key->ptr);
            goto nds_cleanup;
        }
        
        rioInitWithBuffer(&payload, val);
        if (((type = rdbLoadObjectType(&payload)) == -1) ||
            ((obj  = rdbLoadObject(type,&payload)) == NULL))
        {
            redisLog(REDIS_WARNING, "Bad data format for key %s; ignoring", (char *)key->ptr);
            goto nds_cleanup;
        }
    }

nds_cleanup:
    if (val) {
        sdsfree(val);
    }
    return obj;
}

void setNDS(redisDb *db, robj *key, robj *val) {
    rio payload;
    NDSDB *ndsdb;
    
    /* We *can* end up in the situation where setNDS gets called on a key
     * that has been deleted.  Rather than try to special-case that
     * elsewhere (by checking what we get out of lookupKey(), we'll just
     * throw our hands in the air and return early if that's the case.
     */
    if (!val) {
        return;
    }
        
    redisLog(REDIS_DEBUG, "Writing %s to NDS", (char *)key->ptr);
    
    createDumpPayload(&payload, val);
    
    ndsdb = nds_open(db, 1);
    if (!ndsdb) {
        return;
    }
    nds_set(ndsdb, key->ptr, payload.io.buffer.ptr);
    nds_close(ndsdb);
}

/* Delete a key from the NDS.  Returns 0 if the key wasn't found, or
 * 1 if it was.  -1 is returned on error.
 */
int delNDS(redisDb *db, robj *key) {
    NDSDB *ndsdb = nds_open(db, 1);
    int rv = 0;
    
    redisLog(REDIS_DEBUG, "Deleting %s from NDS", (char *)key->ptr);
    
    if (!ndsdb) {
        return -1;
    }
    
    rv = nds_del(ndsdb, key->ptr);
    
    nds_close(ndsdb);

    return rv;
}

/* Return 0/1 based on a key's existence in NDS. */
int existsNDS(redisDb *db, robj *key) {
    NDSDB *ndsdb = nds_open(db, 0);
    int rv;
    
    redisLog(REDIS_DEBUG, "Checking for existence of %s in NDS", key->ptr);
    
    if (!ndsdb) {
        return -1;
    }
    
    rv = nds_exists(ndsdb, key->ptr);
    nds_close(ndsdb);
    
    return rv;
}

/* Walk the entire keyspace of an NDS database, calling walkerCallback for
 * every key we find.  Pass in 'data' for any callback-specific state you
 * might like to deal with.
 */
int walkNDS(redisDb *db,
            int (*walkerCallback)(void *, robj *),
            void *data,
            int interrupt_rate) {
    MDB_cursor *cur = NULL;
    NDSDB *ndsdb = NULL;
    MDB_val key, val;
    int rv, counter = 0;
    
    ndsdb = nds_open(db, 0);
    if (!ndsdb) {
        rv = REDIS_ERR;
        goto cleanup;
    }
    
    rv = mdb_cursor_open(ndsdb->txn, ndsdb->dbi, &cur);
    if (rv) {
        redisLog(REDIS_WARNING, "Failed to open MDB cursor: %s", mdb_strerror(rv));
        rv = REDIS_ERR;
        goto cleanup;
    }
    
    redisLog(REDIS_DEBUG, "Walking the NDS keyspace for DB %i", db->id);
    
    while ((rv = mdb_cursor_get(cur, &key, &val, MDB_NEXT)) == 0) {
        robj *kobj = createStringObject(key.mv_data, key.mv_size);

        if (kobj && walkerCallback(data, kobj) == REDIS_ERR) {
            redisLog(REDIS_DEBUG, "walkNDS terminated prematurely at callback's request");
            rv = REDIS_ERR;
            if (kobj) decrRefCount(kobj);
            goto cleanup;
        }

        if (kobj) decrRefCount(kobj);

        if (interrupt_rate > 0 && !(++counter % interrupt_rate)) {
            /* Let other clients have a sniff */
            aeProcessEvents(server.el, AE_FILE_EVENTS|AE_DONT_WAIT);
        }
    }
    
cleanup:
    if (cur) {
        mdb_cursor_close(cur);
    }
    nds_close(ndsdb);
    
    return rv;
}

/* Clear all NDS databases */
void nukeNDSFromOrbit() {
    redisDb *db;
    
    for (int i = 0; i < server.dbnum; i++) {
        db = server.db+i;
        
        nds_nuke(db);
    }
}

static int preloadWalker(void *data, robj *key) {
    redisDb *db = (redisDb *)data;
    sds copy = sdsdup(key->ptr);

    if (!dictFind(db->dict, copy)) {
        int retval = dictAdd(db->dict, copy, getNDS(db, key));

        redisAssertWithInfo(NULL,key,retval == REDIS_OK);
    }
        
    return REDIS_OK;
}

/* Read all keys from the NDS datastores into memory. */
void preloadNDS() {
    if (server.nds_preload_in_progress || server.nds_preload_complete) {
        return;
    }
    redisLog(REDIS_NOTICE, "Preloading all keys from NDS");
    server.nds_preload_in_progress = 1;
    for (int i = 0; i < server.dbnum; i++) {
        walkNDS(server.db+i, preloadWalker, server.db+i, 1000);
    }
    redisLog(REDIS_NOTICE, "NDS preload complete");
    server.nds_preload_in_progress = 0;
    server.nds_preload_complete = 1;
}

/* Add the key to the dirty keys list if it isn't there already */
void touchDirtyKey(redisDb *db, sds sdskey) {
    sds copy = sdsdup(sdskey);
    dictEntry *de = dictFind(db->dirty_keys, copy);
    
    if (!de) {
        dictAdd(db->dirty_keys, copy, NULL);
    }
}

int isDirtyKey(redisDb *db, sds key) {
    if (dictFind(db->dirty_keys, key)
        || dictFind(db->flushing_keys, key)
       ) {
        return 1;
    } else {
        return 0;
    }
}

unsigned long long dirtyKeyCount() {
    unsigned long long count = 0;
    
    for (int i = 0; i < server.dbnum; i++) {
        count += dictSize((server.db+i)->dirty_keys);
    }
    
    return count;
}

unsigned long long flushingKeyCount() {
    unsigned long long count = 0;
    
    for (int i = 0; i < server.dbnum; i++) {
        count += dictSize((server.db+i)->flushing_keys);
    }
    
    return count;
}
    
/* Fork and flush all the dirty keys out to disk. */
int backgroundDirtyKeysFlush() {
    pid_t childpid;

    if (server.nds_child_pid != -1) return REDIS_ERR;
    
    /* Can't (shouldn't?) happen -- trying to flush while there's already a
     * non-empty set of flushing keys. */
    for (int i = 0; i < server.dbnum; i++) {
        redisDb *db = server.db+i;
        
        if (dictSize(db->flushing_keys) > 0) {
            redisLog(REDIS_WARNING, "FFFUUUUU- you can't flush when there's already keys being flushed.");
            redisLog(REDIS_WARNING, "This isn't supposed to be able to happen.");
            return REDIS_ERR;
        }
    }
    
    server.dirty_before_bgsave = server.dirty;
    
    mdb_env_close(server.mdb_env);
    server.mdb_env = NULL;

    if ((childpid = fork()) == 0) {
        int retval;

        redisLog(REDIS_DEBUG, "In child");
        
        /* Child */
        if (server.ipfd > 0) close(server.ipfd);
        if (server.sofd > 0) close(server.sofd);
        
        redisSetProcTitle("redis-nds-flush");
        
        retval = flushDirtyKeys();
        
        exitFromChild((retval == REDIS_OK) ? 0 : 1);
    } else {
        /* Parent */
        if (childpid == -1) {
            redisLog(REDIS_WARNING, "Can't save in background: fork: %s",
                     strerror(errno));
            return REDIS_ERR;
        }
        
        redisLog(REDIS_DEBUG, "Dirty key flush started in PID %d", childpid);
        server.nds_child_pid = childpid;
        /* Rotate the dirty keys into the flushing keys list, and use the
         * previous flushing keys list as the new dirty keys list. */
        for (int j = 0; j < server.dbnum; j++) {
            redisDb *db = server.db+j;
            dict *dTmp;
            dTmp = db->flushing_keys;
            db->flushing_keys = db->dirty_keys;
            db->dirty_keys = dTmp;
        }
        return REDIS_OK;
    }
    
    /* Can't happen */
    return REDIS_ERR;
}

int flushDirtyKeys() {
    redisLog(REDIS_DEBUG, "Flushing dirty keys");
    for (int j = 0; j < server.dbnum; j++) {
        redisDb *db = server.db+j;
        dictIterator *di;
        dictEntry *deKey, *deVal;
        NDSDB *ndsdb;

        redisLog(REDIS_DEBUG, "Flushing %i keys for DB %i", dictSize(db->dirty_keys), j);
        
        if (!ndsdb) {
            return REDIS_ERR;
        }
        
        if (dictSize(db->dirty_keys) == 0) continue;

        di = dictGetSafeIterator(db->dirty_keys);
        if (!di) {
            redisLog(REDIS_WARNING, "dictGetSafeIterator failed");
            return REDIS_ERR;
        }

	ndsdb = nds_open(db, 1);        
        while ((deKey = dictNext(di)) != NULL) {
            rio payload;
            sds keystr = dictGetKey(deKey);
            deVal = dictFind(db->dict, keystr);
            if (!deVal) {
                /* Key must have been deleted after it got dirtied.  NUKE IT! */
                nds_del(ndsdb, keystr);
            } else {
                createDumpPayload(&payload, dictGetVal(deVal));
                nds_set(ndsdb, keystr, payload.io.buffer.ptr);
                sdsfree(payload.io.buffer.ptr);
            }
        }
        
        nds_close(ndsdb);
    }
    
    redisLog(REDIS_DEBUG, "Flush complete");

    if (server.nds_snapshot_in_progress) {
    	int rv;

    	nds_init();
    	
        /* Woohoo!  Snapshot time! */
        system("rm -rf ./snapshot");
        system("mkdir -p ./snapshot");
        
        if ((rv = mdb_env_copy(server.mdb_env, "./snapshot"))) {
            redisLog(REDIS_WARNING, "Snapshot failed: %s", mdb_strerror(rv));
        }
    }
                
    return REDIS_OK;
}

void backgroundNDSFlushDoneHandler(int exitcode, int bysignal) {
    redisLog(REDIS_NOTICE, "NDS background save completed.  exitcode=%i, bysignal=%i", exitcode, bysignal);

    server.nds_snapshot_in_progress = 0;

    if (exitcode == 0 && bysignal == 0) {
        for (int i = 0; i < server.dbnum; i++) {
            redisDb *db = server.db+i;
            dictEmpty(db->flushing_keys);
        }
        server.dirty -= server.dirty_before_bgsave;
        server.lastsave = time(NULL);
        server.stat_nds_flush_success++;
        
        if (server.nds_bg_requestor) {
            addReply(server.nds_bg_requestor, shared.ok);
            server.nds_bg_requestor = NULL;
        }
    } else {
        server.stat_nds_flush_failure++;
        /* Merge the flushing keys back into the dirty keys so that they'll be
         * retried on the next flush, since we can't know for certain whether
         * they got flushed before our child died */
        for (int i = 0; i < server.dbnum; i++) {
            redisDb *db = server.db+i;
            dictIterator *di;
            dictEntry *de;
        
            di = dictGetSafeIterator(db->flushing_keys);
            if (!di) {
                redisLog(REDIS_WARNING, "backgroundNDSFlushDoneHandler: dictGetSafeIterator failed!  This is terribad!");
                return;
            }
            
            while ((de = dictNext(di)) != NULL) {
                dictAdd(db->dirty_keys, sdsdup(dictGetKey(de)), NULL);
            }
            
            dictEmpty(db->flushing_keys);
        }
        
        if (server.nds_bg_requestor) {
            if (server.nds_snapshot_in_progress) {
                addReplyError(server.nds_bg_requestor, "NDS SNAPSHOT failed in child; consult logs for details");
            } else if (server.nds_bg_requestor) {
                addReplyError(server.nds_bg_requestor, "NDS FLUSH failed in child; consult logs for details");
            }
            server.nds_bg_requestor = NULL;
        }
    }
        
    server.nds_child_pid = -1;
    
    if (server.nds_snapshot_pending) {
        /* Trigger a snapshot job now */
        server.nds_snapshot_in_progress = server.nds_snapshot_pending;
        server.nds_snapshot_pending = 0;
        if (backgroundDirtyKeysFlush() == REDIS_ERR) {
            addReplyError(server.nds_bg_requestor, "Delayed NDS SNAPSHOT failed; consult logs for details");
            return;
        }
    }
}

void checkNDSChildComplete() {
    if (server.nds_child_pid != -1) {
        int statloc;
        pid_t pid;

        if ((pid = wait3(&statloc,WNOHANG,NULL)) != 0) {
            int exitcode = WEXITSTATUS(statloc);
            int bysignal = 0;

            if (pid == -1) {
                redisLog(REDIS_WARNING, "wait3() failed: %s", strerror(errno));
            }

            if (WIFSIGNALED(statloc)) bysignal = WTERMSIG(statloc);

            if (pid > 0) {
                if (pid == server.nds_child_pid) {
                    backgroundNDSFlushDoneHandler(exitcode,bysignal);
                } else {
                    redisLog(REDIS_WARNING,
                        "Warning, detected child with unmatched pid: %ld",
                        (long)pid);
                }
            }
        }
    }
}
                
void ndsFlushCommand(redisClient *c) {
    if (server.nds_bg_requestor) {
        addReplyError(c, "NDS background operation already in progress");
        return;
    }

    server.nds_bg_requestor = c;
    
    if (server.nds_child_pid == -1) {
        if (backgroundDirtyKeysFlush() == REDIS_ERR) {
            addReplyError(c, "NDS FLUSH failed to start; consult logs for details");
            server.nds_bg_requestor = NULL;
            return;
        }
    }
}

void ndsSnapshotCommand(redisClient *c) {
    if (server.nds_snapshot_pending || server.nds_snapshot_in_progress) {
        addReplyError(c, "NDS SNAPSHOT already in progress");
        return;
    }

    if (server.nds_bg_requestor) {
        addReplyError(c, "NDS background operation already in progress");
        return;
    }

    server.nds_bg_requestor = c;
    
    if (server.nds_child_pid == -1) {
        server.nds_snapshot_in_progress = 1;
        if (backgroundDirtyKeysFlush() == REDIS_ERR) {
            addReplyError(c, "NDS SNAPSHOT failed to start; consult logs for details");
            server.nds_bg_requestor = NULL;
            return;
        }
    } else {
    	/* A regular (non-snapshot) NDS flush is already in progress; we'll
         * have to do our snapshot later */
        server.nds_snapshot_pending = 1;
    }
}

void ndsCommand(redisClient *c) {
    if (!strcasecmp(c->argv[1]->ptr,"snapshot")) {
        if (c->argc != 2) goto badarity;
        redisLog(REDIS_NOTICE, "NDS SNAPSHOT requested");
        ndsSnapshotCommand(c);
        /* We don't want to send an OK immediately; that'll get sent when the
         * snapshot completes */
        return;
    } else if (!strcasecmp(c->argv[1]->ptr,"flush")) {
        if (c->argc != 2) goto badarity;
        redisLog(REDIS_NOTICE, "NDS FLUSH requested");
        ndsFlushCommand(c);
        /* We don't want to send an OK immediately; that'll get sent when the
         * flush completes */
        return;
    } else if (!strcasecmp(c->argv[1]->ptr,"clearstats")) {
        if (c->argc != 2) goto badarity;
        redisLog(REDIS_NOTICE, "NDS CLEARSTATS requested");
        server.stat_nds_cache_hits = 0;
        server.stat_nds_cache_misses = 0;
    } else if (!strcasecmp(c->argv[1]->ptr,"preload")) {
        if (c->argc != 2) goto badarity;
        redisLog(REDIS_NOTICE, "NDS PRELOAD requested");
        preloadNDS();
    } else {
        addReplyError(c,
            "NDS subcommand must be one of: SNAPSHOT FLUSH CLEARSTATS PRELOAD");
        return;
    }
    addReply(c, shared.ok);
    return;

badarity:
    addReplyErrorFormat(c,"Wrong number of arguments for NDS %s",
        (char*) c->argv[1]->ptr);
}
