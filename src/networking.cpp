/*
 * Copyright (c) 2009-2012, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2019 John Sully <john at eqalpha dot com>
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

#include "server.h"
#include "atomicvar.h"
#include <sys/socket.h>
#include <sys/uio.h>
#include <math.h>
#include <ctype.h>
#include <vector>
#include <mutex>
#include "aelocker.h"

static void setProtocolError(const char *errstr, client *c);
void addReplyLongLongWithPrefixCore(client *c, long long ll, char prefix, bool fAsync);
void addReplyBulkCStringCore(client *c, const char *s, bool fAsync);

/* Return the size consumed from the allocator, for the specified SDS string,
 * including internal fragmentation. This function is used in order to compute
 * the client output buffer size. */
size_t sdsZmallocSize(sds s) {
    void *sh = sdsAllocPtr(s);
    return zmalloc_size(sh);
}

/* Return the amount of memory used by the sds string at object->ptr
 * for a string object. */
size_t getStringObjectSdsUsedMemory(robj *o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_STRING);
    switch(o->encoding) {
    case OBJ_ENCODING_RAW: return sdsZmallocSize((sds)ptrFromObj(o));
    case OBJ_ENCODING_EMBSTR: return zmalloc_size(o)-sizeof(robj);
    default: return 0; /* Just integer encoding for now. */
    }
}

/* Client.reply list dup and free methods. */
void *dupClientReplyValue(void *o) {
    clientReplyBlock *old = (clientReplyBlock*)o;
    clientReplyBlock *buf = (clientReplyBlock*)zmalloc(sizeof(clientReplyBlock) + old->size, MALLOC_LOCAL);
    memcpy(buf, o, sizeof(clientReplyBlock) + old->size);
    return buf;
}

void freeClientReplyValue(const void *o) {
    zfree(o);
}

int listMatchObjects(void *a, void *b) {
    return equalStringObjects((robj*)a,(robj*)b);
}

/* This function links the client to the global linked list of clients.
 * unlinkClient() does the opposite, among other things. */
void linkClient(client *c) {
    listAddNodeTail(g_pserver->clients,c);
    /* Note that we remember the linked list node where the client is stored,
     * this way removing the client in unlinkClient() will not require
     * a linear scan, but just a constant time operation. */
    c->client_list_node = listLast(g_pserver->clients);
    if (c->fd != -1) atomicIncr(g_pserver->rgthreadvar[c->iel].cclients, 1);
    uint64_t id = htonu64(c->id);
    raxInsert(g_pserver->clients_index,(unsigned char*)&id,sizeof(id),c,NULL);
}

client *createClient(int fd, int iel) {
    client *c = (client*)zmalloc(sizeof(client), MALLOC_LOCAL);

    c->iel = iel;
    /* passing -1 as fd it is possible to create a non connected client.
     * This is useful since all the commands needs to be executed
     * in the context of a client. When commands are executed in other
     * contexts (for instance a Lua script) we need a non connected client. */
    if (fd != -1) {
        anetNonBlock(NULL,fd);
        anetEnableTcpNoDelay(NULL,fd);
        if (cserver.tcpkeepalive)
            anetKeepAlive(NULL,fd,cserver.tcpkeepalive);
        if (aeCreateFileEvent(g_pserver->rgthreadvar[iel].el,fd,AE_READABLE|AE_READ_THREADSAFE,
            readQueryFromClient, c) == AE_ERR)
        {
            close(fd);
            zfree(c);
            return NULL;
        }
    }

    selectDb(c,0);
    uint64_t client_id;
    client_id = g_pserver->next_client_id.fetch_add(1);
    c->iel = iel;
    fastlock_init(&c->lock);
    c->id = client_id;
    c->resp = 2;
    c->fd = fd;
    c->name = NULL;
    c->bufpos = 0;
    c->qb_pos = 0;
    c->querybuf = sdsempty();
    c->pending_querybuf = sdsempty();
    c->querybuf_peak = 0;
    c->reqtype = 0;
    c->argc = 0;
    c->argv = NULL;
    c->cmd = c->lastcmd = NULL;
    c->puser = DefaultUser;
    c->multibulklen = 0;
    c->bulklen = -1;
    c->sentlen = 0;
    c->sentlenAsync = 0;
    c->flags = 0;
    c->fPendingAsyncWrite = FALSE;
    c->ctime = c->lastinteraction = g_pserver->unixtime;
    /* If the default user does not require authentication, the user is
     * directly authenticated. */
    c->authenticated = (c->puser->flags & USER_FLAG_NOPASS) != 0;
    c->replstate = REPL_STATE_NONE;
    c->repl_put_online_on_ack = 0;
    c->reploff = 0;
    c->reploff_skipped = 0;
    c->read_reploff = 0;
    c->repl_ack_off = 0;
    c->repl_ack_time = 0;
    c->slave_listening_port = 0;
    c->slave_ip[0] = '\0';
    c->slave_capa = SLAVE_CAPA_NONE;
    c->reply = listCreate();
    c->reply_bytes = 0;
    c->obuf_soft_limit_reached_time = 0;
    listSetFreeMethod(c->reply,freeClientReplyValue);
    listSetDupMethod(c->reply,dupClientReplyValue);
    c->btype = BLOCKED_NONE;
    c->bpop.timeout = 0;
    c->bpop.keys = dictCreate(&objectKeyHeapPointerValueDictType,NULL);
    c->bpop.target = NULL;
    c->bpop.xread_group = NULL;
    c->bpop.xread_consumer = NULL;
    c->bpop.xread_group_noack = 0;
    c->bpop.numreplicas = 0;
    c->bpop.reploffset = 0;
    c->woff = 0;
    c->watched_keys = listCreate();
    c->pubsub_channels = dictCreate(&objectKeyPointerValueDictType,NULL);
    c->pubsub_patterns = listCreate();
    c->peerid = NULL;
    c->client_list_node = NULL;
    c->bufAsync = NULL;
    c->buflenAsync = 0;
    c->bufposAsync = 0;
    c->client_tracking_redirection = 0;
    c->casyncOpsPending = 0;
    memset(c->uuid, 0, UUID_BINARY_LEN);

    listSetFreeMethod(c->pubsub_patterns,decrRefCountVoid);
    listSetMatchMethod(c->pubsub_patterns,listMatchObjects);
    if (fd != -1) linkClient(c);
    initClientMultiState(c);
    AssertCorrectThread(c);
    return c;
}

/* This funciton puts the client in the queue of clients that should write
 * their output buffers to the socket. Note that it does not *yet* install
 * the write handler, to start clients are put in a queue of clients that need
 * to write, so we try to do that before returning in the event loop (see the
 * handleClientsWithPendingWrites() function).
 * If we fail and there is more data to write, compared to what the socket
 * buffers can hold, then we'll really install the handler. */
void clientInstallWriteHandler(client *c) {
    /* Schedule the client to write the output buffers to the socket only
     * if not already done and, for slaves, if the replica can actually receive
     * writes at this stage. */
    if (!(c->flags & CLIENT_PENDING_WRITE) &&
        (c->replstate == REPL_STATE_NONE ||
         (c->replstate == SLAVE_STATE_ONLINE && !c->repl_put_online_on_ack)))
    {
        AssertCorrectThread(c);
        serverAssert(c->lock.fOwnLock());
        /* Here instead of installing the write handler, we just flag the
         * client and put it into a list of clients that have something
         * to write to the socket. This way before re-entering the event
         * loop, we can try to directly write to the client sockets avoiding
         * a system call. We'll only really install the write handler if
         * we'll not be able to write the whole reply at once. */
        c->flags |= CLIENT_PENDING_WRITE;
        std::unique_lock<fastlock> lockf(g_pserver->rgthreadvar[c->iel].lockPendingWrite);
        g_pserver->rgthreadvar[c->iel].clients_pending_write.push_back(c);
    }
}

void clientInstallAsyncWriteHandler(client *c) {
    serverAssert(GlobalLocksAcquired());
    if (!(c->fPendingAsyncWrite)) {
        c->fPendingAsyncWrite = TRUE;
        listAddNodeHead(serverTL->clients_pending_asyncwrite,c);
    }
}

/* This function is called every time we are going to transmit new data
 * to the client. The behavior is the following:
 *
 * If the client should receive new data (normal clients will) the function
 * returns C_OK, and make sure to install the write handler in our event
 * loop so that when the socket is writable new data gets written.
 *
 * If the client should not receive new data, because it is a fake client
 * (used to load AOF in memory), a master or because the setup of the write
 * handler failed, the function returns C_ERR.
 *
 * The function may return C_OK without actually installing the write
 * event handler in the following cases:
 *
 * 1) The event handler should already be installed since the output buffer
 *    already contains something.
 * 2) The client is a replica but not yet online, so we want to just accumulate
 *    writes in the buffer but not actually sending them yet.
 *
 * Typically gets called every time a reply is built, before adding more
 * data to the clients output buffers. If the function returns C_ERR no
 * data should be appended to the output buffers. */
int prepareClientToWrite(client *c, bool fAsync) {
    fAsync = fAsync && !FCorrectThread(c);  // Not async if we're on the right thread
    serverAssert(FCorrectThread(c) || fAsync);
    serverAssert(c->fd <= 0 || c->lock.fOwnLock());

    if (c->flags & CLIENT_FORCE_REPLY) return C_OK; // FORCE REPLY means we're doing something else with the buffer.
                                                // do not install a write handler

    /* If it's the Lua client we always return ok without installing any
     * handler since there is no socket at all. */
    if (c->flags & (CLIENT_LUA|CLIENT_MODULE)) return C_OK;

    /* CLIENT REPLY OFF / SKIP handling: don't send replies. */
    if (c->flags & (CLIENT_REPLY_OFF|CLIENT_REPLY_SKIP)) return C_ERR;

    /* Masters don't receive replies, unless CLIENT_MASTER_FORCE_REPLY flag
     * is set. */
    if ((c->flags & CLIENT_MASTER) &&
        !(c->flags & CLIENT_MASTER_FORCE_REPLY)) return C_ERR;

    if (c->fd <= 0) return C_ERR; /* Fake client for AOF loading. */

    /* Schedule the client to write the output buffers to the socket, unless
     * it should already be setup to do so (it has already pending data). */
    if (!fAsync && !clientHasPendingReplies(c)) clientInstallWriteHandler(c);
    if (fAsync && !(c->fPendingAsyncWrite)) clientInstallAsyncWriteHandler(c);

    /* Authorize the caller to queue in the output buffer of this client. */
    return C_OK;
}

/* -----------------------------------------------------------------------------
 * Low level functions to add more data to output buffers.
 * -------------------------------------------------------------------------- */

int _addReplyToBuffer(client *c, const char *s, size_t len, bool fAsync) {
    if (c->flags & CLIENT_CLOSE_AFTER_REPLY) return C_OK;

    fAsync = fAsync && !FCorrectThread(c);  // Not async if we're on the right thread
    if (fAsync)
    {
        serverAssert(GlobalLocksAcquired());
        if ((c->buflenAsync - c->bufposAsync) < (int)len)
        {
            int minsize = len + c->bufposAsync;
            c->buflenAsync = std::max(minsize, c->buflenAsync*2 - c->buflenAsync);
            c->bufAsync = (char*)zrealloc(c->bufAsync, c->buflenAsync, MALLOC_LOCAL);
            c->buflenAsync = zmalloc_usable(c->bufAsync);
        }
        memcpy(c->bufAsync+c->bufposAsync,s,len);
        c->bufposAsync += len;
    }
    else
    {
        size_t available = sizeof(c->buf)-c->bufpos;

        /* If there already are entries in the reply list, we cannot
        * add anything more to the static buffer. */
        if (listLength(c->reply) > 0) return C_ERR;

        /* Check that the buffer has enough space available for this string. */
        if (len > available) return C_ERR;

        memcpy(c->buf+c->bufpos,s,len);
        c->bufpos+=len;
    }
    return C_OK;
}

void _addReplyProtoToList(client *c, const char *s, size_t len) {
    if (c->flags & CLIENT_CLOSE_AFTER_REPLY) return;
    AssertCorrectThread(c);

    listNode *ln = listLast(c->reply);
    clientReplyBlock *tail = (clientReplyBlock*) (ln? listNodeValue(ln): NULL);

    /* Note that 'tail' may be NULL even if we have a tail node, becuase when
     * addDeferredMultiBulkLength() is used, it sets a dummy node to NULL just
     * fo fill it later, when the size of the bulk length is set. */

    /* Append to tail string when possible. */
    if (tail) {
        /* Copy the part we can fit into the tail, and leave the rest for a
         * new node */
        size_t avail = tail->size - tail->used;
        size_t copy = avail >= len? len: avail;
        memcpy(tail->buf() + tail->used, s, copy);
        tail->used += copy;
        s += copy;
        len -= copy;
    }
    if (len) {
        /* Create a new node, make sure it is allocated to at
         * least PROTO_REPLY_CHUNK_BYTES */
        size_t size = len < PROTO_REPLY_CHUNK_BYTES? PROTO_REPLY_CHUNK_BYTES: len;
        tail = (clientReplyBlock*)zmalloc(size + sizeof(clientReplyBlock), MALLOC_LOCAL);
        /* take over the allocation's internal fragmentation */
        tail->size = zmalloc_usable(tail) - sizeof(clientReplyBlock);
        tail->used = len;
        memcpy(tail->buf(), s, len);
        listAddNodeTail(c->reply, tail);
        c->reply_bytes += tail->size;
    }
    asyncCloseClientOnOutputBufferLimitReached(c);
}

/* -----------------------------------------------------------------------------
 * Higher level functions to queue data on the client output buffer.
 * The following functions are the ones that commands implementations will call.
 * -------------------------------------------------------------------------- */
void addReplyCore(client *c, robj_roptr obj, bool fAsync) {
    if (prepareClientToWrite(c, fAsync) != C_OK) return;

    if (sdsEncodedObject(obj)) {
        if (_addReplyToBuffer(c,(const char*)ptrFromObj(obj),sdslen((sds)ptrFromObj(obj)),fAsync) != C_OK)
            _addReplyProtoToList(c,(const char*)ptrFromObj(obj),sdslen((sds)ptrFromObj(obj)));
    } else if (obj->encoding == OBJ_ENCODING_INT) {
        /* For integer encoded strings we just convert it into a string
         * using our optimized function, and attach the resulting string
         * to the output buffer. */
        char buf[32];
        size_t len = ll2string(buf,sizeof(buf),(long)ptrFromObj(obj));
        if (_addReplyToBuffer(c,buf,len,fAsync) != C_OK)
            _addReplyProtoToList(c,buf,len);
    } else {
        serverPanic("Wrong obj->encoding in addReply()");
    }
}

/* Add the object 'obj' string representation to the client output buffer. */
void addReply(client *c, robj_roptr obj)
{
    addReplyCore(c, obj, false);
}
void addReplyAsync(client *c, robj_roptr obj)
{
    addReplyCore(c, obj, true);
}

/* Add the SDS 's' string to the client output buffer, as a side effect
 * the SDS string is freed. */
void addReplySdsCore(client *c, sds s, bool fAsync) {
    if (prepareClientToWrite(c, fAsync) != C_OK) {
        /* The caller expects the sds to be free'd. */
        sdsfree(s);
        return;
    }
    if (_addReplyToBuffer(c,s,sdslen(s), fAsync) != C_OK)
        _addReplyProtoToList(c,s,sdslen(s));
    sdsfree(s);
}

void addReplySds(client *c, sds s) {
    addReplySdsCore(c, s, false);
}

void addReplySdsAsync(client *c, sds s) {
    addReplySdsCore(c, s, true);
}

/* This low level function just adds whatever protocol you send it to the
 * client buffer, trying the static buffer initially, and using the string
 * of objects if not possible.
 *
 * It is efficient because does not create an SDS object nor an Redis object
 * if not needed. The object will only be created by calling
 * _addReplyProtoToList() if we fail to extend the existing tail object
 * in the list of objects. */
void addReplyProtoCore(client *c, const char *s, size_t len, bool fAsync) {
    if (prepareClientToWrite(c, fAsync) != C_OK) return;
    if (_addReplyToBuffer(c,s,len,fAsync) != C_OK)
        _addReplyProtoToList(c,s,len);
}

void addReplyProto(client *c, const char *s, size_t len) {
    addReplyProtoCore(c, s, len, false);
}

void addReplyProtoAsync(client *c, const char *s, size_t len) {
    addReplyProtoCore(c, s, len, true);
}

/* Low level function called by the addReplyError...() functions.
 * It emits the protocol for a Redis error, in the form:
 *
 * -ERRORCODE Error Message<CR><LF>
 *
 * If the error code is already passed in the string 's', the error
 * code provided is used, otherwise the string "-ERR " for the generic
 * error code is automatically added. */
void addReplyErrorLengthCore(client *c, const char *s, size_t len, bool fAsync) {
    /* If the string already starts with "-..." then the error code
     * is provided by the caller. Otherwise we use "-ERR". */
    if (!len || s[0] != '-') addReplyProtoCore(c,"-ERR ",5,fAsync);
    addReplyProtoCore(c,s,len,fAsync);
    addReplyProtoCore(c,"\r\n",2,fAsync);

    /* Sometimes it could be normal that a replica replies to a master with
     * an error and this function gets called. Actually the error will never
     * be sent because addReply*() against master clients has no effect...
     * A notable example is:
     *
     *    EVAL 'redis.call("incr",KEYS[1]); redis.call("nonexisting")' 1 x
     *
     * Where the master must propagate the first change even if the second
     * will produce an error. However it is useful to log such events since
     * they are rare and may hint at errors in a script or a bug in Redis. */
    if (c->flags & (CLIENT_MASTER|CLIENT_SLAVE) && !(c->flags & CLIENT_MONITOR)) {
        const char* to = reinterpret_cast<const char*>(c->flags & CLIENT_MASTER? "master": "replica");
        const char* from = reinterpret_cast<const char*>(c->flags & CLIENT_MASTER? "replica": "master");
        const char *cmdname = reinterpret_cast<const char*>(c->lastcmd ? c->lastcmd->name : "<unknown>");
        serverLog(LL_WARNING,"== CRITICAL == This %s is sending an error "
                             "to its %s: '%s' after processing the command "
                             "'%s'", from, to, s, cmdname);
    }
}

void addReplyErrorLength(client *c, const char *s, size_t len)
{
    addReplyErrorLengthCore(c, s, len, false);
}

void addReplyError(client *c, const char *err) {
    addReplyErrorLengthCore(c,err,strlen(err), false);
}

void addReplyErrorAsync(client *c, const char *err) {
    addReplyErrorLengthCore(c, err, strlen(err), true);
}

void addReplyErrorFormat(client *c, const char *fmt, ...) {
    size_t l, j;
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    /* Make sure there are no newlines in the string, otherwise invalid protocol
     * is emitted. */
    l = sdslen(s);
    for (j = 0; j < l; j++) {
        if (s[j] == '\r' || s[j] == '\n') s[j] = ' ';
    }
    addReplyErrorLength(c,s,sdslen(s));
    sdsfree(s);
}

void addReplyStatusLength(client *c, const char *s, size_t len) {
    addReplyProto(c,"+",1);
    addReplyProto(c,s,len);
    addReplyProto(c,"\r\n",2);
}

void addReplyStatus(client *c, const char *status) {
    addReplyStatusLength(c,status,strlen(status));
}

void addReplyStatusFormat(client *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap,fmt);
    sds s = sdscatvprintf(sdsempty(),fmt,ap);
    va_end(ap);
    addReplyStatusLength(c,s,sdslen(s));
    sdsfree(s);
}

/* Adds an empty object to the reply list that will contain the multi bulk
 * length, which is not known when this function is called. */
void *addReplyDeferredLen(client *c) {
    /* Note that we install the write event here even if the object is not
     * ready to be sent, since we are sure that before returning to the
     * event loop setDeferredAggregateLen() will be called. */
    if (prepareClientToWrite(c, false) != C_OK) return NULL;
    listAddNodeTail(c->reply,NULL); /* NULL is our placeholder. */
    return listLast(c->reply);
}

void *addReplyDeferredLenAsync(client *c) {
    if (FCorrectThread(c))
        return addReplyDeferredLen(c);
        
    return (void*)((ssize_t)c->bufposAsync);
}

/* Populate the length object and try gluing it to the next chunk. */
void setDeferredAggregateLen(client *c, void *node, long length, char prefix) {
    listNode *ln = (listNode*)node;
    clientReplyBlock *next;
    char lenstr[128];
    size_t lenstr_len = sprintf(lenstr, "%c%ld\r\n", prefix, length);

    /* Abort when *node is NULL: when the client should not accept writes
     * we return NULL in addReplyDeferredLen() */
    if (node == NULL) return;
    serverAssert(!listNodeValue(ln));

    /* Normally we fill this dummy NULL node, added by addReplyDeferredLen(),
     * with a new buffer structure containing the protocol needed to specify
     * the length of the array following. However sometimes when there is
     * little memory to move, we may instead remove this NULL node, and prefix
     * our protocol in the node immediately after to it, in order to save a
     * write(2) syscall later. Conditions needed to do it:
     *
     * - The next node is non-NULL,
     * - It has enough room already allocated
     * - And not too large (avoid large memmove) */
    if (ln->next != NULL && (next = (clientReplyBlock*)listNodeValue(ln->next)) &&
        next->size - next->used >= lenstr_len &&
        next->used < PROTO_REPLY_CHUNK_BYTES * 4) {
        memmove(next->buf() + lenstr_len, next->buf(), next->used);
        memcpy(next->buf(), lenstr, lenstr_len);
        next->used += lenstr_len;
        listDelNode(c->reply,ln);
    } else {
        /* Create a new node */
        clientReplyBlock *buf = (clientReplyBlock*)zmalloc(lenstr_len + sizeof(clientReplyBlock), MALLOC_LOCAL);
        /* Take over the allocation's internal fragmentation */
        buf->size = zmalloc_usable(buf) - sizeof(clientReplyBlock);
        buf->used = lenstr_len;
        memcpy(buf->buf(), lenstr, lenstr_len);
        listNodeValue(ln) = buf;
        c->reply_bytes += buf->size;
    }
    asyncCloseClientOnOutputBufferLimitReached(c);
}

void setDeferredAggregateLenAsync(client *c, void *node, long length, char prefix)
{
    if (FCorrectThread(c)) {
        setDeferredAggregateLen(c, node, length, prefix);
        return;
    }

    char lenstr[128];
    int lenstr_len = sprintf(lenstr, "%c%ld\r\n", prefix, length);

    ssize_t idxSplice = (ssize_t)node;
    serverAssert(idxSplice <= c->bufposAsync);
    if (c->buflenAsync < (c->bufposAsync + lenstr_len))
    {
        c->buflenAsync = std::max((int)(c->bufposAsync+lenstr_len), c->buflenAsync*2 - c->buflenAsync);
        c->bufAsync = (char*)zrealloc(c->bufAsync, c->buflenAsync, MALLOC_LOCAL);
    }
    
    memmove(c->bufAsync + idxSplice + lenstr_len, c->bufAsync + idxSplice, c->bufposAsync - idxSplice);
    memcpy(c->bufAsync + idxSplice, lenstr, lenstr_len);
    c->bufposAsync += lenstr_len;
}

void setDeferredArrayLen(client *c, void *node, long length) {
    setDeferredAggregateLen(c,node,length,'*');
}

void setDeferredArrayLenAsync(client *c, void *node, long length) {
    setDeferredAggregateLenAsync(c, node, length, '*');
}

void setDeferredMapLen(client *c, void *node, long length) {
    int prefix = c->resp == 2 ? '*' : '%';
    if (c->resp == 2) length *= 2;
    setDeferredAggregateLen(c,node,length,prefix);
}

void setDeferredSetLen(client *c, void *node, long length) {
    int prefix = c->resp == 2 ? '*' : '~';
    setDeferredAggregateLen(c,node,length,prefix);
}

void setDeferredAttributeLen(client *c, void *node, long length) {
    int prefix = c->resp == 2 ? '*' : '|';
    if (c->resp == 2) length *= 2;
    setDeferredAggregateLen(c,node,length,prefix);
}

void setDeferredPushLen(client *c, void *node, long length) {
    int prefix = c->resp == 2 ? '*' : '>';
    setDeferredAggregateLen(c,node,length,prefix);
}

/* Add a double as a bulk reply */
void addReplyDoubleCore(client *c, double d, bool fAsync) {
    if (std::isinf(d)) {
        /* Libc in odd systems (Hi Solaris!) will format infinite in a
         * different way, so better to handle it in an explicit way. */
        if (c->resp == 2) {
            addReplyBulkCStringCore(c, d > 0 ? "inf" : "-inf", fAsync);
        } else {
            addReplyProtoCore(c, d > 0 ? ",inf\r\n" : "-inf\r\n",
                              d > 0 ? 6 : 7, fAsync);
        }
    } else {
        char dbuf[MAX_LONG_DOUBLE_CHARS+3],
             sbuf[MAX_LONG_DOUBLE_CHARS+32];
        int dlen, slen;
        if (c->resp == 2) {
            dlen = snprintf(dbuf,sizeof(dbuf),"%.17g",d);
            slen = snprintf(sbuf,sizeof(sbuf),"$%d\r\n%s\r\n",dlen,dbuf);
            addReplyProtoCore(c,sbuf,slen,fAsync);
        } else {
            dlen = snprintf(dbuf,sizeof(dbuf),",%.17g\r\n",d);
            addReplyProtoCore(c,dbuf,dlen,fAsync);
        }
    }
}

void addReplyDouble(client *c, double d) {
    addReplyDoubleCore(c, d, false);
}

void addReplyDoubleAsync(client *c, double d) {
    addReplyDoubleCore(c, d, true);
}

/* Add a long double as a bulk reply, but uses a human readable formatting
 * of the double instead of exposing the crude behavior of doubles to the
 * dear user. */
void addReplyHumanLongDouble(client *c, long double d) {
    if (c->resp == 2) {
        robj *o = createStringObjectFromLongDouble(d,1);
        addReplyBulk(c,o);
        decrRefCount(o);
    } else {
        char buf[MAX_LONG_DOUBLE_CHARS];
        int len = ld2string(buf,sizeof(buf),d,1);
        addReplyProto(c,",",1);
        addReplyProto(c,buf,len);
        addReplyProto(c,"\r\n",2);
    }
}

/* Add a long long as integer reply or bulk len / multi bulk count.
 * Basically this is used to output <prefix><long long><crlf>. */
void addReplyLongLongWithPrefixCore(client *c, long long ll, char prefix, bool fAsync) {
    char buf[128];
    int len;

    /* Things like $3\r\n or *2\r\n are emitted very often by the protocol
     * so we have a few shared objects to use if the integer is small
     * like it is most of the times. */
    if (prefix == '*' && ll < OBJ_SHARED_BULKHDR_LEN && ll >= 0) {
        addReplyCore(c,shared.mbulkhdr[ll], fAsync);
        return;
    } else if (prefix == '$' && ll < OBJ_SHARED_BULKHDR_LEN && ll >= 0) {
        addReplyCore(c,shared.bulkhdr[ll], fAsync);
        return;
    }

    buf[0] = prefix;
    len = ll2string(buf+1,sizeof(buf)-1,ll);
    buf[len+1] = '\r';
    buf[len+2] = '\n';
    addReplyProtoCore(c,buf,len+3, fAsync);
}

void addReplyLongLongWithPrefix(client *c, long long ll, char prefix) {
    addReplyLongLongWithPrefixCore(c, ll, prefix, false);
}

void addReplyLongLongCore(client *c, long long ll, bool fAsync) {
    if (ll == 0)
        addReplyCore(c,shared.czero, fAsync);
    else if (ll == 1)
        addReplyCore(c,shared.cone, fAsync);
    else
        addReplyLongLongWithPrefixCore(c,ll,':', fAsync);
}

void addReplyLongLong(client *c, long long ll) {
    addReplyLongLongCore(c, ll, false);
}

void addReplyLongLongAsync(client *c, long long ll) {
    addReplyLongLongCore(c, ll, true);
}

void addReplyAggregateLenCore(client *c, long length, int prefix, bool fAsync) {
    if (prefix == '*' && length < OBJ_SHARED_BULKHDR_LEN)
        addReplyCore(c,shared.mbulkhdr[length], fAsync);
    else
        addReplyLongLongWithPrefixCore(c,length,prefix, fAsync);
}

void addReplyAggregateLen(client *c, long length, int prefix) {
    addReplyAggregateLenCore(c, length, prefix, false);
}

void addReplyArrayLenCore(client *c, long length, bool fAsync) {
    addReplyAggregateLenCore(c,length,'*', fAsync);
}

void addReplyArrayLen(client *c, long length) {
    addReplyArrayLenCore(c, length, false);
}

void addReplyArrayLenAsync(client *c, long length) {
    addReplyArrayLenCore(c, length, true);
}

void addReplyMapLenCore(client *c, long length, bool fAsync) {
    int prefix = c->resp == 2 ? '*' : '%';
    if (c->resp == 2) length *= 2;
    addReplyAggregateLenCore(c,length,prefix,fAsync);
}

void addReplyMapLen(client *c, long length) {
    addReplyMapLenCore(c, length, false);
}

void addReplyMapLenAsync(client *c, long length) {
    addReplyMapLenCore(c, length, true);
}

void addReplySetLen(client *c, long length) {
    int prefix = c->resp == 2 ? '*' : '~';
    addReplyAggregateLen(c,length,prefix);
}

void addReplyAttributeLen(client *c, long length) {
    int prefix = c->resp == 2 ? '*' : '|';
    if (c->resp == 2) length *= 2;
    addReplyAggregateLen(c,length,prefix);
}

void addReplyPushLenCore(client *c, long length, bool fAsync) {
    int prefix = c->resp == 2 ? '*' : '>';
    addReplyAggregateLenCore(c,length,prefix, fAsync);
}

void addReplyPushLen(client *c, long length) {
    addReplyPushLenCore(c, length, false);
}

void addReplyPushLenAsync(client *c, long length) {
    addReplyPushLenCore(c, length, true);
}

void addReplyNullCore(client *c, bool fAsync) {
    if (c->resp == 2) {
        addReplyProtoCore(c,"$-1\r\n",5,fAsync);
    } else {
        addReplyProtoCore(c,"_\r\n",3,fAsync);
    }
}

void addReplyNull(client *c, robj_roptr objOldProtocol) {
    if (c->resp < 3 && objOldProtocol != nullptr)
        addReply(c, objOldProtocol);
    else
        addReplyNullCore(c, false);
}

void addReplyNullAsync(client *c) {
    addReplyNullCore(c, true);
}

void addReplyBool(client *c, int b) {
    if (c->resp == 2) {
        addReply(c, b ? shared.cone : shared.czero);
    } else {
        addReplyProto(c, b ? "#t\r\n" : "#f\r\n",4);
    }
}

/* A null array is a concept that no longer exists in RESP3. However
 * RESP2 had it, so API-wise we have this call, that will emit the correct
 * RESP2 protocol, however for RESP3 the reply will always be just the
 * Null type "_\r\n". */
void addReplyNullArray(client *c) {
    if (c->resp == 2) {
        addReplyProto(c,"*-1\r\n",5);
    } else {
        addReplyProto(c,"_\r\n",3);
    }
}

/* Create the length prefix of a bulk reply, example: $2234 */
void addReplyBulkLenCore(client *c, robj_roptr obj, bool fAsync) {
    size_t len = stringObjectLen(obj);

    if (len < OBJ_SHARED_BULKHDR_LEN)
        addReplyCore(c,shared.bulkhdr[len], fAsync);
    else
        addReplyLongLongWithPrefixCore(c,len,'$', fAsync);
}

void addReplyBulkLen(client *c, robj *obj)
{
    addReplyBulkLenCore(c, obj, false);
}

/* Add a Redis Object as a bulk reply */
void addReplyBulkCore(client *c, robj_roptr obj, bool fAsync) {
    addReplyBulkLenCore(c,obj,fAsync);
    addReplyCore(c,obj,fAsync);
    addReplyCore(c,shared.crlf,fAsync);
}

void addReplyBulk(client *c, robj_roptr obj)
{
    addReplyBulkCore(c, obj, false);
}

void addReplyBulkAsync(client *c, robj_roptr obj)
{
    addReplyBulkCore(c, obj, true);
}

/* Add a C buffer as bulk reply */
void addReplyBulkCBufferCore(client *c, const void *p, size_t len, bool fAsync) {
    addReplyLongLongWithPrefixCore(c,len,'$',fAsync);
    addReplyProtoCore(c,(const char*)p,len,fAsync);
    addReplyCore(c,shared.crlf,fAsync);
}

void addReplyBulkCBuffer(client *c, const void *p, size_t len) {
    addReplyBulkCBufferCore(c, p, len, false);
}

void addReplyBulkCBufferAsync(client *c, const void *p, size_t len) {
    addReplyBulkCBufferCore(c, p, len, true);
}

/* Add sds to reply (takes ownership of sds and frees it) */
void addReplyBulkSdsCore(client *c, sds s, bool fAsync)  {
    addReplyLongLongWithPrefixCore(c,sdslen(s),'$', fAsync);
    addReplySdsCore(c,s,fAsync);
    addReplyCore(c,shared.crlf,fAsync);
}

void addReplyBulkSds(client *c, sds s) {
    addReplyBulkSdsCore(c, s, false);
}

void addReplyBulkSdsAsync(client *c, sds s) {
    addReplyBulkSdsCore(c, s, true);
}

/* Add a C null term string as bulk reply */
void addReplyBulkCStringCore(client *c, const char *s, bool fAsync) {
    if (s == NULL) {
        if (c->resp < 3)
            addReplyCore(c,shared.nullbulk, fAsync);
        else
            addReplyNullCore(c,fAsync);
    } else {
        addReplyBulkCBufferCore(c,s,strlen(s),fAsync);
    }
}

void addReplyBulkCString(client *c, const char *s) {
    addReplyBulkCStringCore(c, s, false);
}

/* Add a long long as a bulk reply */
void addReplyBulkLongLong(client *c, long long ll) {
    char buf[64];
    int len;

    len = ll2string(buf,64,ll);
    addReplyBulkCBuffer(c,buf,len);
}

/* Reply with a verbatim type having the specified extension.
 *
 * The 'ext' is the "extension" of the file, actually just a three
 * character type that describes the format of the verbatim string.
 * For instance "txt" means it should be interpreted as a text only
 * file by the receiver, "md " as markdown, and so forth. Only the
 * three first characters of the extension are used, and if the
 * provided one is shorter than that, the remaining is filled with
 * spaces. */
void addReplyVerbatim(client *c, const char *s, size_t len, const char *ext) {
    if (c->resp == 2) {
        addReplyBulkCBuffer(c,s,len);
    } else {
        char buf[32];
        size_t preflen = snprintf(buf,sizeof(buf),"=%zu\r\nxxx:",len+4);
        char *p = buf+preflen-4;
        for (int i = 0; i < 3; i++) {
            if (*ext == '\0') {
                p[i] = ' ';
            } else {
                p[i] = *ext++;
            }
        }
        addReplyProto(c,buf,preflen);
        addReplyProto(c,s,len);
        addReplyProto(c,"\r\n",2);
    }
}

/* Add an array of C strings as status replies with a heading.
 * This function is typically invoked by from commands that support
 * subcommands in response to the 'help' subcommand. The help array
 * is terminated by NULL sentinel. */
void addReplyHelp(client *c, const char **help) {
    sds cmd = sdsnew((char*) ptrFromObj(c->argv[0]));
    void *blenp = addReplyDeferredLen(c);
    int blen = 0;

    sdstoupper(cmd);
    addReplyStatusFormat(c,
        "%s <subcommand> arg arg ... arg. Subcommands are:",cmd);
    sdsfree(cmd);

    while (help[blen]) addReplyStatus(c,help[blen++]);

    blen++;  /* Account for the header line(s). */
    setDeferredArrayLen(c,blenp,blen);
}

/* Add a suggestive error reply.
 * This function is typically invoked by from commands that support
 * subcommands in response to an unknown subcommand or argument error. */
void addReplySubcommandSyntaxError(client *c) {
    sds cmd = sdsnew((char*) ptrFromObj(c->argv[0]));
    sdstoupper(cmd);
    addReplyErrorFormat(c,
        "Unknown subcommand or wrong number of arguments for '%s'. Try %s HELP.",
        (char*)ptrFromObj(c->argv[1]),cmd);
    sdsfree(cmd);
}

/* Append 'src' client output buffers into 'dst' client output buffers. 
 * This function clears the output buffers of 'src' */
void AddReplyFromClient(client *dst, client *src) {
    if (prepareClientToWrite(dst, false) != C_OK)
        return;
    addReplyProto(dst,src->buf, src->bufpos);
    if (listLength(src->reply))
        listJoin(dst->reply,src->reply);
    dst->reply_bytes += src->reply_bytes;
    src->reply_bytes = 0;
    src->bufpos = 0;
}

/* Copy 'src' client output buffers into 'dst' client output buffers.
 * The function takes care of freeing the old output buffers of the
 * destination client. */
void copyClientOutputBuffer(client *dst, client *src) {
    listRelease(dst->reply);
    dst->sentlen = 0;
    dst->reply = listDup(src->reply);
    memcpy(dst->buf,src->buf,src->bufpos);
    dst->bufpos = src->bufpos;
    dst->reply_bytes = src->reply_bytes;
}

/* Return true if the specified client has pending reply buffers to write to
 * the socket. */
int clientHasPendingReplies(client *c) {
    return (c->bufpos || listLength(c->reply)) && !(c->flags & CLIENT_CLOSE_ASAP);
}

#define MAX_ACCEPTS_PER_CALL 1000
static void acceptCommonHandler(int fd, int flags, char *ip, int iel) {
    client *c;
    if ((c = createClient(fd, iel)) == NULL) {
        serverLog(LL_WARNING,
            "Error registering fd event for the new client: %s (fd=%d)",
            strerror(errno),fd);
        return;
    }

#ifdef HAVE_SO_INCOMING_CPU
    // Set thread affinity
    if (cserver.fThreadAffinity)
    {
        int cpu = iel;
        if (setsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, &cpu, sizeof(iel)) != 0)
        {
            serverLog(LL_WARNING, "Failed to set socket affinity");
        }
    }
#endif

    /* If maxclient directive is set and this is one client more... close the
     * connection. Note that we create the client instead to check before
     * for this condition, since now the socket is already set in non-blocking
     * mode and we can send an error for free using the Kernel I/O */
    if (listLength(g_pserver->clients) > g_pserver->maxclients) {
        const char *err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors */
        if (write(c->fd,err,strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        g_pserver->stat_rejected_conn++;
        freeClient(c);
        return;
    }

    /* If the server is running in protected mode (the default) and there
     * is no password set, nor a specific interface is bound, we don't accept
     * requests from non loopback interfaces. Instead we try to explain the
     * user what to do to fix it if needed. */
    if (g_pserver->protected_mode &&
        g_pserver->bindaddr_count == 0 &&
        DefaultUser->flags & USER_FLAG_NOPASS &&
        !(flags & CLIENT_UNIX_SOCKET) &&
        ip != NULL)
    {
        if (strcmp(ip,"127.0.0.1") && strcmp(ip,"::1")) {
            const char *err =
                "-DENIED Redis is running in protected mode because protected "
                "mode is enabled, no bind address was specified, no "
                "authentication password is requested to clients. In this mode "
                "connections are only accepted from the loopback interface. "
                "If you want to connect from external computers to Redis you "
                "may adopt one of the following solutions: "
                "1) Just disable protected mode sending the command "
                "'CONFIG SET protected-mode no' from the loopback interface "
                "by connecting to Redis from the same host the server is "
                "running, however MAKE SURE Redis is not publicly accessible "
                "from internet if you do so. Use CONFIG REWRITE to make this "
                "change permanent. "
                "2) Alternatively you can just disable the protected mode by "
                "editing the Redis configuration file, and setting the protected "
                "mode option to 'no', and then restarting the g_pserver-> "
                "3) If you started the server manually just for testing, restart "
                "it with the '--protected-mode no' option. "
                "4) Setup a bind address or an authentication password. "
                "NOTE: You only need to do one of the above things in order for "
                "the server to start accepting connections from the outside.\r\n";
            if (write(c->fd,err,strlen(err)) == -1) {
                /* Nothing to do, Just to avoid the warning... */
            }
            g_pserver->stat_rejected_conn++;
            freeClient(c);
            return;
        }
    }

    g_pserver->stat_numconnections++;
    c->flags |= flags;
}

void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd, max = MAX_ACCEPTS_PER_CALL;
    char cip[NET_IP_STR_LEN];
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        cfd = anetTcpAccept(serverTL->neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", serverTL->neterr);
            return;
        }
        serverLog(LL_VERBOSE,"Accepted %s:%d", cip, cport);
        int ielCur = ielFromEventLoop(el);

        if (!g_fTestMode)
        {
            // We always accept on the same thread
        LLocalThread:
            aeAcquireLock();
            acceptCommonHandler(cfd,0,cip, ielCur);
            aeReleaseLock();
        }
        else
        {
            // In test mode we want a good distribution among threads and avoid the main thread
            //  since the main thread is most likely to work
            int iel = IDX_EVENT_LOOP_MAIN;
            while (cserver.cthreads > 1 && iel == IDX_EVENT_LOOP_MAIN)
                iel = rand() % cserver.cthreads;
            if (iel == ielFromEventLoop(el))
                goto LLocalThread;
            char *szT = (char*)zmalloc(NET_IP_STR_LEN, MALLOC_LOCAL);
            memcpy(szT, cip, NET_IP_STR_LEN);
            aePostFunction(g_pserver->rgthreadvar[iel].el, [cfd, iel, szT]{
                acceptCommonHandler(cfd,0,szT, iel);
                zfree(szT);
            });
        }
    }
}

void acceptUnixHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cfd, max = MAX_ACCEPTS_PER_CALL;
    UNUSED(el);
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        cfd = anetUnixAccept(serverTL->neterr, fd);
        if (cfd == ANET_ERR) {
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", serverTL->neterr);
            return;
        }
        int ielCur = ielFromEventLoop(el);
        serverLog(LL_VERBOSE,"Accepted connection to %s", g_pserver->unixsocket);

        aeAcquireLock();
        int ielTarget = rand() % cserver.cthreads;
        if (ielTarget == ielCur)
        {
            acceptCommonHandler(cfd,CLIENT_UNIX_SOCKET,NULL, ielCur);
        }
        else
        {
            aePostFunction(g_pserver->rgthreadvar[ielTarget].el, [cfd, ielTarget]{
                acceptCommonHandler(cfd,CLIENT_UNIX_SOCKET,NULL, ielTarget);
            });
        }
        aeReleaseLock();
        
    }
}

static void freeClientArgv(client *c) {
    int j;
    for (j = 0; j < c->argc; j++)
        decrRefCount(c->argv[j]);
    c->argc = 0;
    c->cmd = NULL;
}

void disconnectSlavesExcept(unsigned char *uuid)
{
    serverAssert(GlobalLocksAcquired());
    listIter li;
    listNode *ln;

    listRewind(g_pserver->slaves, &li);
    while ((ln = listNext(&li))) {
        client *c = (client*)listNodeValue(ln);
        if (uuid == nullptr || !FUuidEqual(c->uuid, uuid))
            freeClientAsync(c);
    }   
}

/* Close all the slaves connections. This is useful in chained replication
 * when we resync with our own master and want to force all our slaves to
 * resync with us as well. */
void disconnectSlaves(void) {
    disconnectSlavesExcept(nullptr);
}

/* Remove the specified client from global lists where the client could
 * be referenced, not including the Pub/Sub channels.
 * This is used by freeClient() and replicationCacheMaster(). */
void unlinkClient(client *c) {
    listNode *ln;
    AssertCorrectThread(c);
    serverAssert(c->fd == -1 || GlobalLocksAcquired());
    serverAssert(c->fd == -1 || c->lock.fOwnLock());

    /* If this is marked as current client unset it. */
    if (serverTL && serverTL->current_client == c) serverTL->current_client = NULL;

    /* Certain operations must be done only if the client has an active socket.
     * If the client was already unlinked or if it's a "fake client" the
     * fd is already set to -1. */
    if (c->fd != -1) {
        /* Remove from the list of active clients. */
        if (c->client_list_node) {
            uint64_t id = htonu64(c->id);
            raxRemove(g_pserver->clients_index,(unsigned char*)&id,sizeof(id),NULL);
            listDelNode(g_pserver->clients,c->client_list_node);
            c->client_list_node = NULL;
        }

        /* In the case of diskless replication the fork is writing to the
         * sockets and just closing the fd isn't enough, if we don't also
         * shutdown the socket the fork will continue to write to the replica
         * and the salve will only find out that it was disconnected when
         * it will finish reading the rdb. */
        if ((c->flags & CLIENT_SLAVE) &&
            (c->replstate == SLAVE_STATE_WAIT_BGSAVE_END)) {
            shutdown(c->fd, SHUT_RDWR);
        }

        /* Unregister async I/O handlers and close the socket. */
        aeDeleteFileEvent(g_pserver->rgthreadvar[c->iel].el,c->fd,AE_READABLE);
        aeDeleteFileEvent(g_pserver->rgthreadvar[c->iel].el,c->fd,AE_WRITABLE);
        close(c->fd);
        c->fd = -1;

        atomicDecr(g_pserver->rgthreadvar[c->iel].cclients, 1);
    }

    /* Remove from the list of pending writes if needed. */
    if (c->flags & CLIENT_PENDING_WRITE) {
        std::unique_lock<fastlock> lockf(g_pserver->rgthreadvar[c->iel].lockPendingWrite);
        auto itr = std::find(g_pserver->rgthreadvar[c->iel].clients_pending_write.begin(),
            g_pserver->rgthreadvar[c->iel].clients_pending_write.end(), c);
        serverAssert(itr != g_pserver->rgthreadvar[c->iel].clients_pending_write.end());
        g_pserver->rgthreadvar[c->iel].clients_pending_write.erase(itr);
        c->flags &= ~CLIENT_PENDING_WRITE;
    }

    /* When client was just unblocked because of a blocking operation,
     * remove it from the list of unblocked clients. */
    if (c->flags & CLIENT_UNBLOCKED) {
        ln = listSearchKey(g_pserver->rgthreadvar[c->iel].unblocked_clients,c);
        serverAssert(ln != NULL);
        listDelNode(g_pserver->rgthreadvar[c->iel].unblocked_clients,ln);
        c->flags &= ~CLIENT_UNBLOCKED;
    }

    if (c->fPendingAsyncWrite) {
        ln = NULL;
        bool fFound = false;
        for (int iel = 0; iel < cserver.cthreads; ++iel)
        {
            ln = listSearchKey(g_pserver->rgthreadvar[iel].clients_pending_asyncwrite,c);
            if (ln)
            {
                fFound = true;
                listDelNode(g_pserver->rgthreadvar[iel].clients_pending_asyncwrite,ln);
            }
        }
        serverAssert(fFound);
        c->fPendingAsyncWrite = FALSE;
    }

    /* Clear the tracking status. */
    if (c->flags & CLIENT_TRACKING) disableTracking(c);
}

bool freeClient(client *c) {
    listNode *ln;
    serverAssert(c->fd == -1 || GlobalLocksAcquired());
    AssertCorrectThread(c);
    std::unique_lock<decltype(c->lock)> ulock(c->lock);

    /* If a client is protected, yet we need to free it right now, make sure
     * to at least use asynchronous freeing. */
    if (c->flags & CLIENT_PROTECTED || c->casyncOpsPending) {
        freeClientAsync(c);
        return false;
    }

    /* If it is our master that's beging disconnected we should make sure
     * to cache the state to try a partial resynchronization later.
     *
     * Note that before doing this we make sure that the client is not in
     * some unexpected state, by checking its flags. */
    if (FActiveMaster(c)) {
        serverLog(LL_WARNING,"Connection with master lost.");
        if (!(c->flags & (CLIENT_CLOSE_AFTER_REPLY|
                        CLIENT_CLOSE_ASAP|
                        CLIENT_BLOCKED)))
        {
            replicationCacheMaster(MasterInfoFromClient(c), c);
            return false;
        }
    }

    /* Log link disconnection with replica */
    if ((c->flags & CLIENT_SLAVE) && !(c->flags & CLIENT_MONITOR)) {
        serverLog(LL_WARNING,"Connection with replica %s lost.",
            replicationGetSlaveName(c));
    }

    /* Free the query buffer */
    sdsfree(c->querybuf);
    sdsfree(c->pending_querybuf);
    c->querybuf = NULL;

    /* Deallocate structures used to block on blocking ops. */
    if (c->flags & CLIENT_BLOCKED) unblockClient(c);
    dictRelease(c->bpop.keys);

    /* UNWATCH all the keys */
    unwatchAllKeys(c);
    listRelease(c->watched_keys);

    /* Unsubscribe from all the pubsub channels */
    pubsubUnsubscribeAllChannels(c,0);
    pubsubUnsubscribeAllPatterns(c,0);
    dictRelease(c->pubsub_channels);
    listRelease(c->pubsub_patterns);

    /* Free data structures. */
    listRelease(c->reply);
    freeClientArgv(c);

    /* Unlink the client: this will close the socket, remove the I/O
     * handlers, and remove references of the client from different
     * places where active clients may be referenced. */
    unlinkClient(c);

    /* Master/replica cleanup Case 1:
     * we lost the connection with a replica. */
    if (c->flags & CLIENT_SLAVE) {
        if (c->replstate == SLAVE_STATE_SEND_BULK) {
            if (c->repldbfd != -1) close(c->repldbfd);
            if (c->replpreamble) sdsfree(c->replpreamble);
        }
        list *l = (c->flags & CLIENT_MONITOR) ? g_pserver->monitors : g_pserver->slaves;
        ln = listSearchKey(l,c);
        serverAssert(ln != NULL);
        listDelNode(l,ln);
        /* We need to remember the time when we started to have zero
         * attached slaves, as after some time we'll free the replication
         * backlog. */
        if (c->flags & CLIENT_SLAVE && listLength(g_pserver->slaves) == 0)
            g_pserver->repl_no_slaves_since = g_pserver->unixtime;
        refreshGoodSlavesCount();
    }

    /* Master/replica cleanup Case 2:
     * we lost the connection with the master. */
    if (c->flags & CLIENT_MASTER) replicationHandleMasterDisconnection(MasterInfoFromClient(c));

    /* If this client was scheduled for async freeing we need to remove it
     * from the queue. */
    if (c->flags & CLIENT_CLOSE_ASAP) {
        ln = listSearchKey(g_pserver->clients_to_close,c);
        serverAssert(ln != NULL);
        listDelNode(g_pserver->clients_to_close,ln);
    }

    /* Release other dynamically allocated client structure fields,
     * and finally release the client structure itself. */
    zfree(c->bufAsync);
    if (c->name) decrRefCount(c->name);
    zfree(c->argv);
    freeClientMultiState(c);
    sdsfree(c->peerid);
    ulock.unlock();
    fastlock_free(&c->lock);
    zfree(c);
    return true;
}

/* Schedule a client to free it at a safe time in the serverCron() function.
 * This function is useful when we need to terminate a client but we are in
 * a context where calling freeClient() is not possible, because the client
 * should be valid for the continuation of the flow of the program. */
void freeClientAsync(client *c) {
    /* We need to handle concurrent access to the server.clients_to_close list
     * only in the freeClientAsync() function, since it's the only function that
     * may access the list while Redis uses I/O threads. All the other accesses
     * are in the context of the main thread while the other threads are
     * idle. */
    if (c->flags & CLIENT_CLOSE_ASAP || c->flags & CLIENT_LUA) return;  // check without the lock first
    std::lock_guard<decltype(c->lock)> clientlock(c->lock);
    AeLocker lock;
    lock.arm(c);
    if (c->flags & CLIENT_CLOSE_ASAP || c->flags & CLIENT_LUA) return;  // race condition after we acquire the lock
    c->flags |= CLIENT_CLOSE_ASAP;    
    listAddNodeTail(g_pserver->clients_to_close,c);
}

void freeClientsInAsyncFreeQueue(int iel) {
    serverAssert(GlobalLocksAcquired());
    listIter li;
    listNode *ln;
    listRewind(g_pserver->clients_to_close,&li);

    // Store the clients in a temp vector since freeClient will modify this list
    std::vector<client*> vecclientsFree;
    while((ln = listNext(&li))) 
    {
        client *c = (client*)listNodeValue(ln);
        if (c->iel == iel)
        {
            vecclientsFree.push_back(c);
            listDelNode(g_pserver->clients_to_close, ln);
        }
    }

    for (client *c : vecclientsFree)
    {
        c->flags &= ~CLIENT_CLOSE_ASAP;
        freeClient(c);
    }
}

/* Return a client by ID, or NULL if the client ID is not in the set
 * of registered clients. Note that "fake clients", created with -1 as FD,
 * are not registered clients. */
client *lookupClientByID(uint64_t id) {
    id = htonu64(id);
    client *c = (client*)raxFind(g_pserver->clients_index,(unsigned char*)&id,sizeof(id));
    return (c == raxNotFound) ? NULL : c;
}

/* Write data in output buffers to client. Return C_OK if the client
 * is still valid after the call, C_ERR if it was freed because of some
 * error.
 *
 * This function is called by threads, but always with handler_installed
 * set to 0. So when handler_installed is set to 0 the function must be
 * thread safe. */
int writeToClient(int fd, client *c, int handler_installed) {
    ssize_t nwritten = 0, totwritten = 0;
    clientReplyBlock *o;
    AssertCorrectThread(c);

    std::unique_lock<decltype(c->lock)> lock(c->lock);
   
    while(clientHasPendingReplies(c)) {
        if (c->bufpos > 0) {
            nwritten = write(fd,c->buf+c->sentlen,c->bufpos-c->sentlen);

            if (nwritten <= 0) break;
            c->sentlen += nwritten;
            totwritten += nwritten;

            /* If the buffer was sent, set bufpos to zero to continue with
             * the remainder of the reply. */
            if ((int)c->sentlen == c->bufpos) {
                c->bufpos = 0;
                c->sentlen = 0;
            }
        } else {
            o = (clientReplyBlock*)listNodeValue(listFirst(c->reply));
            if (o->used == 0) {
                c->reply_bytes -= o->size;
                listDelNode(c->reply,listFirst(c->reply));
                continue;
            }

            nwritten = write(fd, o->buf() + c->sentlen, o->used - c->sentlen);
            if (nwritten <= 0)
                break;
                
            c->sentlen += nwritten;
            totwritten += nwritten;
            
            /* If we fully sent the object on head go to the next one */
            if (c->sentlen == o->used) {
                c->reply_bytes -= o->size;
                listDelNode(c->reply,listFirst(c->reply));
                c->sentlen = 0;
                /* If there are no longer objects in the list, we expect
                    * the count of reply bytes to be exactly zero. */
                if (listLength(c->reply) == 0)
                    serverAssert(c->reply_bytes == 0);
            }
        }
        /* Note that we avoid to send more than NET_MAX_WRITES_PER_EVENT
         * bytes, in a single threaded server it's a good idea to serve
         * other clients as well, even if a very large request comes from
         * super fast link that is always able to accept data (in real world
         * scenario think about 'KEYS *' against the loopback interface).
         *
         * However if we are over the maxmemory limit we ignore that and
         * just deliver as much data as it is possible to deliver.
         *
         * Moreover, we also send as much as possible if the client is
         * a replica (otherwise, on high-speed traffic, the replication
         * buffer will grow indefinitely) */
        if (totwritten > NET_MAX_WRITES_PER_EVENT &&
            (g_pserver->maxmemory == 0 ||
             zmalloc_used_memory() < g_pserver->maxmemory) &&
            !(c->flags & CLIENT_SLAVE)) break;
    }
    
    g_pserver->stat_net_output_bytes += totwritten;
    if (nwritten == -1) {
        if (errno == EAGAIN) {
            nwritten = 0;
        } else {
            serverLog(LL_VERBOSE,
                "Error writing to client: %s", strerror(errno));
            lock.unlock();
            freeClientAsync(c);
            
            return C_ERR;
        }
    }
    if (totwritten > 0) {
        /* For clients representing masters we don't count sending data
         * as an interaction, since we always send REPLCONF ACK commands
         * that take some time to just fill the socket output buffer.
         * We just rely on data / pings received for timeout detection. */
        if (!(c->flags & CLIENT_MASTER)) c->lastinteraction = g_pserver->unixtime;
    }
    if (!clientHasPendingReplies(c)) {
        c->sentlen = 0;
        if (handler_installed) aeDeleteFileEvent(g_pserver->rgthreadvar[c->iel].el,c->fd,AE_WRITABLE);

        /* Close connection after entire reply has been sent. */
        if (c->flags & CLIENT_CLOSE_AFTER_REPLY) {
            lock.unlock();
            freeClientAsync(c);
            return C_ERR;
        }
    }
    return C_OK;
}

/* Write event handler. Just send data to the client. */
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    UNUSED(mask);
    client *c = (client*)privdata;

    serverAssert(ielFromEventLoop(el) == c->iel);
    if (writeToClient(fd,c,1) == C_ERR)
    {
        AeLocker ae;
        c->lock.lock();
        ae.arm(c);
        if (c->flags & CLIENT_CLOSE_ASAP)
            freeClient(c);
    }
}

void ProcessPendingAsyncWrites()
{
    if (serverTL == nullptr)
        return; // module fake call

    serverAssert(GlobalLocksAcquired());

    while(listLength(serverTL->clients_pending_asyncwrite)) {
        client *c = (client*)listNodeValue(listFirst(serverTL->clients_pending_asyncwrite));
        listDelNode(serverTL->clients_pending_asyncwrite, listFirst(serverTL->clients_pending_asyncwrite));
        std::lock_guard<decltype(c->lock)> lock(c->lock);

        serverAssert(c->fPendingAsyncWrite);
        if (c->flags & (CLIENT_CLOSE_ASAP | CLIENT_CLOSE_AFTER_REPLY))
        {
            c->bufposAsync = 0;
            c->buflenAsync = 0;
            zfree(c->bufAsync);
            c->bufAsync = nullptr;
            c->fPendingAsyncWrite = FALSE;
            continue;
        }

        // TODO: Append to end of reply block?

        size_t size = c->bufposAsync;
        clientReplyBlock *reply = (clientReplyBlock*)zmalloc(size + sizeof(clientReplyBlock), MALLOC_LOCAL);
        /* take over the allocation's internal fragmentation */
        reply->size = zmalloc_usable(reply) - sizeof(clientReplyBlock);
        reply->used = c->bufposAsync;
        memcpy(reply->buf(), c->bufAsync, c->bufposAsync);
        listAddNodeTail(c->reply, reply);
        c->reply_bytes += reply->size;

        c->bufposAsync = 0;
        c->buflenAsync = 0;
        zfree(c->bufAsync);
        c->bufAsync = nullptr;
        c->fPendingAsyncWrite = FALSE;

        // Now install the write event handler
        int ae_flags = AE_WRITABLE|AE_WRITE_THREADSAFE;
        /* For the fsync=always policy, we want that a given FD is never
            * served for reading and writing in the same event loop iteration,
            * so that in the middle of receiving the query, and serving it
            * to the client, we'll call beforeSleep() that will do the
            * actual fsync of AOF to disk. AE_BARRIER ensures that. */
        if (g_pserver->aof_state == AOF_ON &&
            g_pserver->aof_fsync == AOF_FSYNC_ALWAYS)
        {
            ae_flags |= AE_BARRIER;
        }
        
        if (!((c->replstate == REPL_STATE_NONE ||
         (c->replstate == SLAVE_STATE_ONLINE && !c->repl_put_online_on_ack))))
            continue;

        asyncCloseClientOnOutputBufferLimitReached(c);
        if (c->flags & CLIENT_CLOSE_ASAP)
            continue;   // we will never write this so don't post an op
        
        std::atomic_thread_fence(std::memory_order_seq_cst);
        
        if (c->casyncOpsPending == 0)
        {
            if (FCorrectThread(c))
            {
                prepareClientToWrite(c, false); // queue an event
            }
            else
            {
                // We need to start the write on the client's thread
                if (aePostFunction(g_pserver->rgthreadvar[c->iel].el, [c]{
                        // Install a write handler.  Don't do the actual write here since we don't want
                        //  to duplicate the throttling and safety mechanisms of the normal write code
                        std::lock_guard<decltype(c->lock)> lock(c->lock);
                        serverAssert(c->casyncOpsPending > 0);
                        c->casyncOpsPending--;
                        aeCreateFileEvent(g_pserver->rgthreadvar[c->iel].el, c->fd, AE_WRITABLE|AE_WRITE_THREADSAFE, sendReplyToClient, c);
                    }, false) == AE_ERR
                )
                {
                    // Posting the function failed
                    continue;   // We can retry later in the cron
                }
                ++c->casyncOpsPending; // race is handled by the client lock in the lambda
            }
        }
    }
}

/* This function is called just before entering the event loop, in the hope
 * we can just write the replies to the client output buffer without any
 * need to use a syscall in order to install the writable event handler,
 * get it called, and so forth. */
int handleClientsWithPendingWrites(int iel) {
    std::unique_lock<fastlock> lockf(g_pserver->rgthreadvar[iel].lockPendingWrite);
    auto &vec = g_pserver->rgthreadvar[iel].clients_pending_write;
    int processed = (int)vec.size();
    serverAssert(iel == (serverTL - g_pserver->rgthreadvar));

    int ae_flags = AE_WRITABLE|AE_WRITE_THREADSAFE;
    /* For the fsync=always policy, we want that a given FD is never
        * served for reading and writing in the same event loop iteration,
        * so that in the middle of receiving the query, and serving it
        * to the client, we'll call beforeSleep() that will do the
        * actual fsync of AOF to disk. AE_BARRIER ensures that. */
    if (g_pserver->aof_state == AOF_ON &&
        g_pserver->aof_fsync == AOF_FSYNC_ALWAYS)
    {
        ae_flags |= AE_BARRIER;
    }

    while(!vec.empty()) {
        client *c = vec.back();
        AssertCorrectThread(c);

        c->flags &= ~CLIENT_PENDING_WRITE;
        vec.pop_back();

        /* If a client is protected, don't do anything,
         * that may trigger write error or recreate handler. */
        if (c->flags & CLIENT_PROTECTED) continue;

        std::unique_lock<decltype(c->lock)> lock(c->lock);

        /* Try to write buffers to the client socket. */
        if (writeToClient(c->fd,c,0) == C_ERR) 
        {
            if (c->flags & CLIENT_CLOSE_ASAP)
            {
                lock.release(); // still locked
                AeLocker ae;
                ae.arm(c);
                if (!freeClient(c))  // writeToClient will only async close, but there's no need to wait
                    c->lock.unlock();   // if we just got put on the async close list, then we need to remove the lock
            }
            continue;
        }

        /* If after the synchronous writes above we still have data to
         * output to the client, we need to install the writable handler. */
        if (clientHasPendingReplies(c)) {
            if (aeCreateFileEvent(g_pserver->rgthreadvar[c->iel].el, c->fd, ae_flags, sendReplyToClient, c) == AE_ERR)
                freeClientAsync(c);
        }
    }

    if (listLength(serverTL->clients_pending_asyncwrite))
    {
        AeLocker locker;
        locker.arm(nullptr);
        ProcessPendingAsyncWrites();
    }

    return processed;
}

/* resetClient prepare the client to process the next command */
void resetClient(client *c) {
    redisCommandProc *prevcmd = c->cmd ? c->cmd->proc : NULL;

    freeClientArgv(c);
    c->reqtype = 0;
    c->multibulklen = 0;
    c->bulklen = -1;

    /* We clear the ASKING flag as well if we are not inside a MULTI, and
     * if what we just executed is not the ASKING command itself. */
    if (!(c->flags & CLIENT_MULTI) && prevcmd != askingCommand)
        c->flags &= ~CLIENT_ASKING;

    /* Remove the CLIENT_REPLY_SKIP flag if any so that the reply
     * to the next command will be sent, but set the flag if the command
     * we just processed was "CLIENT REPLY SKIP". */
    c->flags &= ~CLIENT_REPLY_SKIP;
    if (c->flags & CLIENT_REPLY_SKIP_NEXT) {
        c->flags |= CLIENT_REPLY_SKIP;
        c->flags &= ~CLIENT_REPLY_SKIP_NEXT;
    }
}

/* This funciton is used when we want to re-enter the event loop but there
 * is the risk that the client we are dealing with will be freed in some
 * way. This happens for instance in:
 *
 * * DEBUG RELOAD and similar.
 * * When a Lua script is in -BUSY state.
 *
 * So the function will protect the client by doing two things:
 *
 * 1) It removes the file events. This way it is not possible that an
 *    error is signaled on the socket, freeing the client.
 * 2) Moreover it makes sure that if the client is freed in a different code
 *    path, it is not really released, but only marked for later release. */
void protectClient(client *c) {
    c->flags |= CLIENT_PROTECTED;
    AssertCorrectThread(c);
    aeDeleteFileEvent(g_pserver->rgthreadvar[c->iel].el,c->fd,AE_READABLE);
    aeDeleteFileEvent(g_pserver->rgthreadvar[c->iel].el,c->fd,AE_WRITABLE);
}

/* This will undo the client protection done by protectClient() */
void unprotectClient(client *c) {
    AssertCorrectThread(c);
    if (c->flags & CLIENT_PROTECTED) {
        c->flags &= ~CLIENT_PROTECTED;
        aeCreateFileEvent(g_pserver->rgthreadvar[c->iel].el,c->fd,AE_READABLE|AE_READ_THREADSAFE,readQueryFromClient,c);
        if (clientHasPendingReplies(c)) clientInstallWriteHandler(c);
    }
}

/* Like processMultibulkBuffer(), but for the inline protocol instead of RESP,
 * this function consumes the client query buffer and creates a command ready
 * to be executed inside the client structure. Returns C_OK if the command
 * is ready to be executed, or C_ERR if there is still protocol to read to
 * have a well formed command. The function also returns C_ERR when there is
 * a protocol error: in such a case the client structure is setup to reply
 * with the error and close the connection. */
int processInlineBuffer(client *c) {
    char *newline;
    int argc, j, linefeed_chars = 1;
    sds *argv, aux;
    size_t querylen;

    /* Search for end of line */
    newline = strchr(c->querybuf+c->qb_pos,'\n');

    /* Nothing to do without a \r\n */
    if (newline == NULL) {
        if (sdslen(c->querybuf)-c->qb_pos > PROTO_INLINE_MAX_SIZE) {
            addReplyError(c,"Protocol error: too big inline request");
            setProtocolError("too big inline request",c);
        }
        return C_ERR;
    }

    /* Handle the \r\n case. */
    if (newline && newline != c->querybuf+c->qb_pos && *(newline-1) == '\r')
        newline--, linefeed_chars++;

    /* Split the input buffer up to the \r\n */
    querylen = newline-(c->querybuf+c->qb_pos);
    aux = sdsnewlen(c->querybuf+c->qb_pos,querylen);
    argv = sdssplitargs(aux,&argc);
    sdsfree(aux);
    if (argv == NULL) {
        addReplyError(c,"Protocol error: unbalanced quotes in request");
        setProtocolError("unbalanced quotes in inline request",c);
        return C_ERR;
    }

    /* Newline from slaves can be used to refresh the last ACK time.
     * This is useful for a replica to ping back while loading a big
     * RDB file. */
    if (querylen == 0 && c->flags & CLIENT_SLAVE)
        c->repl_ack_time = g_pserver->unixtime;

    /* Move querybuffer position to the next query in the buffer. */
    c->qb_pos += querylen+linefeed_chars;

    /* Setup argv array on client structure */
    if (argc) {
        if (c->argv) zfree(c->argv);
        c->argv = (robj**)zmalloc(sizeof(robj*)*argc, MALLOC_LOCAL);
    }

    /* Create redis objects for all arguments. */
    for (c->argc = 0, j = 0; j < argc; j++) {
        if (sdslen(argv[j])) {
            c->argv[c->argc] = createObject(OBJ_STRING,argv[j]);
            c->argc++;
        } else {
            sdsfree(argv[j]);
        }
    }
    sds_free(argv);
    return C_OK;
}

/* Helper function. Record protocol erro details in server log,
 * and set the client as CLIENT_CLOSE_AFTER_REPLY. */
#define PROTO_DUMP_LEN 128
static void setProtocolError(const char *errstr, client *c) {
    if (cserver.verbosity <= LL_VERBOSE) {
        sds client = catClientInfoString(sdsempty(),c);

        /* Sample some protocol to given an idea about what was inside. */
        char buf[256];
        if (sdslen(c->querybuf)-c->qb_pos < PROTO_DUMP_LEN) {
            snprintf(buf,sizeof(buf),"Query buffer during protocol error: '%s'", c->querybuf+c->qb_pos);
        } else {
            snprintf(buf,sizeof(buf),"Query buffer during protocol error: '%.*s' (... more %zu bytes ...) '%.*s'", PROTO_DUMP_LEN/2, c->querybuf+c->qb_pos, sdslen(c->querybuf)-c->qb_pos-PROTO_DUMP_LEN, PROTO_DUMP_LEN/2, c->querybuf+sdslen(c->querybuf)-PROTO_DUMP_LEN/2);
        }

        /* Remove non printable chars. */
        char *p = buf;
        while (*p != '\0') {
            if (!isprint(*p)) *p = '.';
            p++;
        }

        /* Log all the client and protocol info. */
        serverLog(LL_VERBOSE,
            "Protocol error (%s) from client: %s. %s", errstr, client, buf);
        sdsfree(client);
    }
    c->flags |= CLIENT_CLOSE_AFTER_REPLY;
}

/* Process the query buffer for client 'c', setting up the client argument
 * vector for command execution. Returns C_OK if after running the function
 * the client has a well-formed ready to be processed command, otherwise
 * C_ERR if there is still to read more buffer to get the full command.
 * The function also returns C_ERR when there is a protocol error: in such a
 * case the client structure is setup to reply with the error and close
 * the connection.
 *
 * This function is called if processInputBuffer() detects that the next
 * command is in RESP format, so the first byte in the command is found
 * to be '*'. Otherwise for inline commands processInlineBuffer() is called. */
int processMultibulkBuffer(client *c) {
    char *newline = NULL;
    int ok;
    long long ll;

    if (c->multibulklen == 0) {
        /* The client should have been reset */
        serverAssertWithInfo(c,NULL,c->argc == 0);

        /* Multi bulk length cannot be read without a \r\n */
        newline = strchr(c->querybuf+c->qb_pos,'\r');
        if (newline == NULL) {
            if (sdslen(c->querybuf)-c->qb_pos > PROTO_INLINE_MAX_SIZE) {
                addReplyError(c,"Protocol error: too big mbulk count string");
                setProtocolError("too big mbulk count string",c);
            }
            return C_ERR;
        }

        /* Buffer should also contain \n */
        if (newline-(c->querybuf+c->qb_pos) > (ssize_t)(sdslen(c->querybuf)-c->qb_pos-2))
            return C_ERR;

        /* We know for sure there is a whole line since newline != NULL,
         * so go ahead and find out the multi bulk length. */
        serverAssertWithInfo(c,NULL,c->querybuf[c->qb_pos] == '*');
        ok = string2ll(c->querybuf+1+c->qb_pos,newline-(c->querybuf+1+c->qb_pos),&ll);
        if (!ok || ll > 1024*1024) {
            addReplyError(c,"Protocol error: invalid multibulk length");
            setProtocolError("invalid mbulk count",c);
            return C_ERR;
        }

        c->qb_pos = (newline-c->querybuf)+2;

        if (ll <= 0) return C_OK;

        c->multibulklen = ll;

        /* Setup argv array on client structure */
        if (c->argv) zfree(c->argv);
        c->argv = (robj**)zmalloc(sizeof(robj*)*c->multibulklen, MALLOC_LOCAL);
    }

    serverAssertWithInfo(c,NULL,c->multibulklen > 0);
    while(c->multibulklen) {
        /* Read bulk length if unknown */
        if (c->bulklen == -1) {
            newline = strchr(c->querybuf+c->qb_pos,'\r');
            if (newline == NULL) {
                if (sdslen(c->querybuf)-c->qb_pos > PROTO_INLINE_MAX_SIZE) {
                    addReplyError(c,
                        "Protocol error: too big bulk count string");
                    setProtocolError("too big bulk count string",c);
                    return C_ERR;
                }
                break;
            }

            /* Buffer should also contain \n */
            if (newline-(c->querybuf+c->qb_pos) > (ssize_t)(sdslen(c->querybuf)-c->qb_pos-2))
                break;

            if (c->querybuf[c->qb_pos] != '$') {
                addReplyErrorFormat(c,
                    "Protocol error: expected '$', got '%c'",
                    c->querybuf[c->qb_pos]);
                setProtocolError("expected $ but got something else",c);
                return C_ERR;
            }

            ok = string2ll(c->querybuf+c->qb_pos+1,newline-(c->querybuf+c->qb_pos+1),&ll);
            if (!ok || ll < 0 || ll > g_pserver->proto_max_bulk_len) {
                addReplyError(c,"Protocol error: invalid bulk length");
                setProtocolError("invalid bulk length",c);
                return C_ERR;
            }

            c->qb_pos = newline-c->querybuf+2;
            if (ll >= PROTO_MBULK_BIG_ARG) {
                /* If we are going to read a large object from network
                 * try to make it likely that it will start at c->querybuf
                 * boundary so that we can optimize object creation
                 * avoiding a large copy of data.
                 *
                 * But only when the data we have not parsed is less than
                 * or equal to ll+2. If the data length is greater than
                 * ll+2, trimming querybuf is just a waste of time, because
                 * at this time the querybuf contains not only our bulk. */
                if (sdslen(c->querybuf)-c->qb_pos <= (size_t)ll+2) {
                    sdsrange(c->querybuf,c->qb_pos,-1);
                    c->qb_pos = 0;
                    /* Hint the sds library about the amount of bytes this string is
                     * going to contain. */
                    c->querybuf = sdsMakeRoomFor(c->querybuf,ll+2);
                }
            }
            c->bulklen = ll;
        }

        /* Read bulk argument */
        if (sdslen(c->querybuf)-c->qb_pos < (size_t)(c->bulklen+2)) {
            /* Not enough data (+2 == trailing \r\n) */
            break;
        } else {
            /* Optimization: if the buffer contains JUST our bulk element
             * instead of creating a new object by *copying* the sds we
             * just use the current sds string. */
            if (c->qb_pos == 0 &&
                c->bulklen >= PROTO_MBULK_BIG_ARG &&
                sdslen(c->querybuf) == (size_t)(c->bulklen+2))
            {
                c->argv[c->argc++] = createObject(OBJ_STRING,c->querybuf);
                sdsIncrLen(c->querybuf,-2); /* remove CRLF */
                /* Assume that if we saw a fat argument we'll see another one
                 * likely... */
                c->querybuf = sdsnewlen(SDS_NOINIT,c->bulklen+2);
                sdsclear(c->querybuf);
            } else {
                c->argv[c->argc++] =
                    createStringObject(c->querybuf+c->qb_pos,c->bulklen);
                c->qb_pos += c->bulklen+2;
            }
            c->bulklen = -1;
            c->multibulklen--;
        }
    }

    /* We're done when c->multibulk == 0 */
    if (c->multibulklen == 0) return C_OK;

    /* Still not ready to process the command */
    return C_ERR;
}

/* This function calls processCommand(), but also performs a few sub tasks
 * that are useful in that context:
 *
 * 1. It sets the current client to the client 'c'.
 * 2. In the case of master clients, the replication offset is updated.
 * 3. The client is reset unless there are reasons to avoid doing it.
 *
 * The function returns C_ERR in case the client was freed as a side effect
 * of processing the command, otherwise C_OK is returned. */
int processCommandAndResetClient(client *c, int flags) {
    int deadclient = 0;
    serverTL->current_client = c;
    if (processCommand(c, flags) == C_OK) {
        if (c->flags & CLIENT_MASTER && !(c->flags & CLIENT_MULTI)) {
            /* Update the applied replication offset of our master. */
            c->reploff = c->read_reploff - sdslen(c->querybuf) + c->qb_pos;
        }

        /* Don't reset the client structure for clients blocked in a
         * module blocking command, so that the reply callback will
         * still be able to access the client argv and argc field.
         * The client will be reset in unblockClientFromModule(). */
        if (!(c->flags & CLIENT_BLOCKED) ||
            c->btype != BLOCKED_MODULE)
        {
            resetClient(c);
        }
    }
    if (serverTL->current_client == NULL) deadclient = 1;
    serverTL->current_client = NULL;
    /* freeMemoryIfNeeded may flush replica output buffers. This may
     * result into a replica, that may be the active client, to be
     * freed. */
    return deadclient ? C_ERR : C_OK;
}

/* This function is called every time, in the client structure 'c', there is
 * more query buffer to process, because we read more data from the socket
 * or because a client was blocked and later reactivated, so there could be
 * pending query buffer, already representing a full command, to process. */
void processInputBuffer(client *c, int callFlags) {
    AssertCorrectThread(c);
    
    /* Keep processing while there is something in the input buffer */
    while(c->qb_pos < sdslen(c->querybuf)) {
        /* Return if clients are paused. */
        if (!(c->flags & CLIENT_SLAVE) && clientsArePaused()) break;

        /* Immediately abort if the client is in the middle of something. */
        if (c->flags & CLIENT_BLOCKED) break;

        /* Don't process input from the master while there is a busy script
         * condition on the replica. We want just to accumulate the replication
         * stream (instead of replying -BUSY like we do with other clients) and
         * later resume the processing. */
        if (g_pserver->lua_timedout && c->flags & CLIENT_MASTER) break;

        /* CLIENT_CLOSE_AFTER_REPLY closes the connection once the reply is
         * written to the client. Make sure to not let the reply grow after
         * this flag has been set (i.e. don't process more commands).
         *
         * The same applies for clients we want to terminate ASAP. */
        if (c->flags & (CLIENT_CLOSE_AFTER_REPLY|CLIENT_CLOSE_ASAP)) break;

        /* Determine request type when unknown. */
        if (!c->reqtype) {
            if (c->querybuf[c->qb_pos] == '*') {
                c->reqtype = PROTO_REQ_MULTIBULK;
            } else {
                c->reqtype = PROTO_REQ_INLINE;
            }
        }

        if (c->reqtype == PROTO_REQ_INLINE) {
            if (processInlineBuffer(c) != C_OK) break;
        } else if (c->reqtype == PROTO_REQ_MULTIBULK) {
            if (processMultibulkBuffer(c) != C_OK) break;
        } else {
            serverPanic("Unknown request type");
        }

        /* Multibulk processing could see a <= 0 length. */
        if (c->argc == 0) {
            resetClient(c);
        } else {
            /* We are finally ready to execute the command. */
            if (processCommandAndResetClient(c, callFlags) == C_ERR) {
                /* If the client is no longer valid, we avoid exiting this
                 * loop and trimming the client buffer later. So we return
                 * ASAP in that case. */
                return;
            }
        }
    }

    /* Trim to pos */
    if (c->qb_pos) {
        sdsrange(c->querybuf,c->qb_pos,-1);
        c->qb_pos = 0;
    }
}

/* This is a wrapper for processInputBuffer that also cares about handling
 * the replication forwarding to the sub-replicas, in case the client 'c'
 * is flagged as master. Usually you want to call this instead of the
 * raw processInputBuffer(). */
void processInputBufferAndReplicate(client *c) {
    if (!(c->flags & CLIENT_MASTER)) {
        processInputBuffer(c, CMD_CALL_FULL);
    } else {
        /* If the client is a master we need to compute the difference
         * between the applied offset before and after processing the buffer,
         * to understand how much of the replication stream was actually
         * applied to the master state: this quantity, and its corresponding
         * part of the replication stream, will be propagated to the
         * sub-replicas and to the replication backlog. */
        size_t prev_offset = c->reploff;
        processInputBuffer(c, CMD_CALL_FULL);
        size_t applied = c->reploff - prev_offset;
        if (applied) {
            if (!g_pserver->fActiveReplica)
            {
                AeLocker ae;
                ae.arm(c);
                replicationFeedSlavesFromMasterStream(g_pserver->slaves,
                        c->pending_querybuf, applied);
            }
            sdsrange(c->pending_querybuf,applied,-1);
        }
    }
}

void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask) {
    client *c = (client*) privdata;
    int nread, readlen;
    size_t qblen;
    UNUSED(el);
    UNUSED(mask);
    serverAssert(mask & AE_READ_THREADSAFE);
    serverAssert(c->iel == ielFromEventLoop(el));
    
    AeLocker aelock;
    AssertCorrectThread(c);
    std::unique_lock<decltype(c->lock)> lock(c->lock, std::defer_lock);
    if (!lock.try_lock())
        return; // Process something else while we wait

    readlen = PROTO_IOBUF_LEN;
    /* If this is a multi bulk request, and we are processing a bulk reply
     * that is large enough, try to maximize the probability that the query
     * buffer contains exactly the SDS string representing the object, even
     * at the risk of requiring more read(2) calls. This way the function
     * processMultiBulkBuffer() can avoid copying buffers to create the
     * Redis Object representing the argument. */
    if (c->reqtype == PROTO_REQ_MULTIBULK && c->multibulklen && c->bulklen != -1
        && c->bulklen >= PROTO_MBULK_BIG_ARG)
    {
        ssize_t remaining = (size_t)(c->bulklen+2)-sdslen(c->querybuf);

        /* Note that the 'remaining' variable may be zero in some edge case,
         * for example once we resume a blocked client after CLIENT PAUSE. */
        if (remaining > 0 && remaining < readlen) readlen = remaining;
    }

    qblen = sdslen(c->querybuf);
    if (c->querybuf_peak < qblen) c->querybuf_peak = qblen;
    c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);
    
    nread = read(fd, c->querybuf+qblen, readlen);
    
    if (nread == -1) {
        if (errno == EAGAIN) {
            return;
        } else {
            serverLog(LL_VERBOSE, "Reading from client: %s",strerror(errno));
            freeClientAsync(c);
            return;
        }
    } else if (nread == 0) {
        serverLog(LL_VERBOSE, "Client closed connection");
        freeClientAsync(c);
        return;
    } else if (c->flags & CLIENT_MASTER) {
        /* Append the query buffer to the pending (not applied) buffer
         * of the master. We'll use this buffer later in order to have a
         * copy of the string applied by the last command executed. */
        c->pending_querybuf = sdscatlen(c->pending_querybuf,
                                        c->querybuf+qblen,nread);
    }

    sdsIncrLen(c->querybuf,nread);
    c->lastinteraction = g_pserver->unixtime;
    if (c->flags & CLIENT_MASTER) c->read_reploff += nread;
    g_pserver->stat_net_input_bytes += nread;
    if (sdslen(c->querybuf) > cserver.client_max_querybuf_len) {
        sds ci = catClientInfoString(sdsempty(),c), bytes = sdsempty();

        bytes = sdscatrepr(bytes,c->querybuf,64);
        serverLog(LL_WARNING,"Closing client that reached max query buffer length: %s (qbuf initial bytes: %s)", ci, bytes);
        sdsfree(ci);
        sdsfree(bytes);       
        freeClientAsync(c);
        return;
    }

    /* Time to process the buffer. If the client is a master we need to
     * compute the difference between the applied offset before and after
     * processing the buffer, to understand how much of the replication stream
     * was actually applied to the master state: this quantity, and its
     * corresponding part of the replication stream, will be propagated to
     * the sub-slaves and to the replication backlog. */
    processInputBufferAndReplicate(c);
    if (listLength(serverTL->clients_pending_asyncwrite))
    {
        aelock.arm(c);
        ProcessPendingAsyncWrites();
    }
}

void getClientsMaxBuffers(unsigned long *longest_output_list,
                          unsigned long *biggest_input_buffer) {
    client *c;
    listNode *ln;
    listIter li;
    unsigned long lol = 0, bib = 0;

    listRewind(g_pserver->clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        c = (client*)listNodeValue(ln);

        if (listLength(c->reply) > lol) lol = listLength(c->reply);
        if (sdslen(c->querybuf) > bib) bib = sdslen(c->querybuf);
    }
    *longest_output_list = lol;
    *biggest_input_buffer = bib;
}

/* A Redis "Peer ID" is a colon separated ip:port pair.
 * For IPv4 it's in the form x.y.z.k:port, example: "127.0.0.1:1234".
 * For IPv6 addresses we use [] around the IP part, like in "[::1]:1234".
 * For Unix sockets we use path:0, like in "/tmp/redis:0".
 *
 * A Peer ID always fits inside a buffer of NET_PEER_ID_LEN bytes, including
 * the null term.
 *
 * On failure the function still populates 'peerid' with the "?:0" string
 * in case you want to relax error checking or need to display something
 * anyway (see anetPeerToString implementation for more info). */
void genClientPeerId(client *client, char *peerid,
                            size_t peerid_len) {
    if (client->flags & CLIENT_UNIX_SOCKET) {
        /* Unix socket client. */
        snprintf(peerid,peerid_len,"%s:0",g_pserver->unixsocket);
    } else {
        /* TCP client. */
        anetFormatPeer(client->fd,peerid,peerid_len);
    }
}

/* This function returns the client peer id, by creating and caching it
 * if client->peerid is NULL, otherwise returning the cached value.
 * The Peer ID never changes during the life of the client, however it
 * is expensive to compute. */
char *getClientPeerId(client *c) {
    char peerid[NET_PEER_ID_LEN];

    if (c->peerid == NULL) {
        genClientPeerId(c,peerid,sizeof(peerid));
        c->peerid = sdsnew(peerid);
    }
    return c->peerid;
}

/* Concatenate a string representing the state of a client in an human
 * readable format, into the sds string 's'. */
sds catClientInfoString(sds s, client *client) {
    char flags[16], events[3], *p;
    int emask;

    p = flags;
    if (client->flags & CLIENT_SLAVE) {
        if (client->flags & CLIENT_MONITOR)
            *p++ = 'O';
        else
            *p++ = 'S';
    }
    if (client->flags & CLIENT_MASTER) *p++ = 'M';
    if (client->flags & CLIENT_PUBSUB) *p++ = 'P';
    if (client->flags & CLIENT_MULTI) *p++ = 'x';
    if (client->flags & CLIENT_BLOCKED) *p++ = 'b';
    if (client->flags & CLIENT_TRACKING) *p++ = 't';
    if (client->flags & CLIENT_TRACKING_BROKEN_REDIR) *p++ = 'R';
    if (client->flags & CLIENT_DIRTY_CAS) *p++ = 'd';
    if (client->flags & CLIENT_CLOSE_AFTER_REPLY) *p++ = 'c';
    if (client->flags & CLIENT_UNBLOCKED) *p++ = 'u';
    if (client->flags & CLIENT_CLOSE_ASAP) *p++ = 'A';
    if (client->flags & CLIENT_UNIX_SOCKET) *p++ = 'U';
    if (client->flags & CLIENT_READONLY) *p++ = 'r';
    if (p == flags) *p++ = 'N';
    *p++ = '\0';

    emask = client->fd == -1 ? 0 : aeGetFileEvents(g_pserver->rgthreadvar[client->iel].el,client->fd);
    p = events;
    if (emask & AE_READABLE) *p++ = 'r';
    if (emask & AE_WRITABLE) *p++ = 'w';
    *p = '\0';
    return sdscatfmt(s,
        "id=%U addr=%s fd=%i name=%s age=%I idle=%I flags=%s db=%i sub=%i psub=%i multi=%i qbuf=%U qbuf-free=%U obl=%U oll=%U omem=%U events=%s cmd=%s",
        (unsigned long long) client->id,
        getClientPeerId(client),
        client->fd,
        client->name ? (char*)ptrFromObj(client->name) : "",
        (long long)(g_pserver->unixtime - client->ctime),
        (long long)(g_pserver->unixtime - client->lastinteraction),
        flags,
        client->db->id,
        (int) dictSize(client->pubsub_channels),
        (int) listLength(client->pubsub_patterns),
        (client->flags & CLIENT_MULTI) ? client->mstate.count : -1,
        (unsigned long long) sdslen(client->querybuf),
        (unsigned long long) sdsavail(client->querybuf),
        (unsigned long long) client->bufpos,
        (unsigned long long) listLength(client->reply),
        (unsigned long long) getClientOutputBufferMemoryUsage(client),
        events,
        client->lastcmd ? client->lastcmd->name : "NULL");
}

sds getAllClientsInfoString(int type) {
    listNode *ln;
    listIter li;
    client *client;
    sds o = sdsnewlen(SDS_NOINIT,200*listLength(g_pserver->clients));
    sdsclear(o);
    listRewind(g_pserver->clients,&li);
    while ((ln = listNext(&li)) != NULL) {
        client = reinterpret_cast<struct client*>(listNodeValue(ln));
        std::unique_lock<decltype(client->lock)> lock(client->lock);
        if (type != -1 && getClientType(client) != type) continue;
        o = catClientInfoString(o,client);
        o = sdscatlen(o,"\n",1);
    }
    return o;
}

/* This function implements CLIENT SETNAME, including replying to the
 * user with an error if the charset is wrong (in that case C_ERR is
 * returned). If the function succeeeded C_OK is returned, and it's up
 * to the caller to send a reply if needed.
 *
 * Setting an empty string as name has the effect of unsetting the
 * currently set name: the client will remain unnamed.
 *
 * This function is also used to implement the HELLO SETNAME option. */
int clientSetNameOrReply(client *c, robj *name) {
    int len = sdslen((sds)ptrFromObj(name));
    char *p = (char*)ptrFromObj(name);

    /* Setting the client name to an empty string actually removes
     * the current name. */
    if (len == 0) {
        if (c->name) decrRefCount(c->name);
        c->name = NULL;
        addReply(c,shared.ok);
        return C_OK;
    }

    /* Otherwise check if the charset is ok. We need to do this otherwise
     * CLIENT LIST format will break. You should always be able to
     * split by space to get the different fields. */
    for (int j = 0; j < len; j++) {
        if (p[j] < '!' || p[j] > '~') { /* ASCII is assumed. */
            addReplyError(c,
                "Client names cannot contain spaces, "
                "newlines or special characters.");
            return C_ERR;
        }
    }
    if (c->name) decrRefCount(c->name);
    c->name = name;
    incrRefCount(name);
    return C_OK;
}

void clientCommand(client *c) {
    listNode *ln;
    listIter li;
    client *client;

    if (c->argc == 2 && !strcasecmp((const char*)ptrFromObj(c->argv[1]),"help")) {
        const char *help[] = {
"id                     -- Return the ID of the current connection.",
"getname                -- Return the name of the current connection.",
"kill <ip:port>         -- Kill connection made from <ip:port>.",
"kill <option> <value> [option value ...] -- Kill connections. Options are:",
"     addr <ip:port>                      -- Kill connection made from <ip:port>",
"     type (normal|master|replica|pubsub) -- Kill connections by type.",
"     skipme (yes|no)   -- Skip killing current connection (default: yes).",
"list [options ...]     -- Return information about client connections. Options:",
"     type (normal|master|replica|pubsub) -- Return clients of specified type.",
"pause <timeout>        -- Suspend all Redis clients for <timout> milliseconds.",
"reply (on|off|skip)    -- Control the replies sent to the current connection.",
"setname <name>         -- Assign the name <name> to the current connection.",
"unblock <clientid> [TIMEOUT|ERROR] -- Unblock the specified blocked client.",
"tracking (on|off) [REDIRECT <id>] -- Enable client keys tracking for client side caching.",
NULL
        };
        addReplyHelp(c, help);
    } else if (!strcasecmp((const char*)ptrFromObj(c->argv[1]),"id") && c->argc == 2) {
        /* CLIENT ID */
        addReplyLongLong(c,c->id);
    } else if (!strcasecmp((const char*)ptrFromObj(c->argv[1]),"list")) {
        /* CLIENT LIST */
        int type = -1;
        if (c->argc == 4 && !strcasecmp((const char*)ptrFromObj(c->argv[2]),"type")) {
            type = getClientTypeByName((char*)ptrFromObj(c->argv[3]));
            if (type == -1) {
                addReplyErrorFormat(c,"Unknown client type '%s'",
                    (char*) ptrFromObj(c->argv[3]));
                return;
             }
        } else if (c->argc != 2) {
            addReply(c,shared.syntaxerr);
            return;
        }
        sds o = getAllClientsInfoString(type);
        addReplyBulkCBuffer(c,o,sdslen(o));
        sdsfree(o);
    } else if (!strcasecmp((const char*)ptrFromObj(c->argv[1]),"reply") && c->argc == 3) {
        /* CLIENT REPLY ON|OFF|SKIP */
        if (!strcasecmp((const char*)ptrFromObj(c->argv[2]),"on")) {
            c->flags &= ~(CLIENT_REPLY_SKIP|CLIENT_REPLY_OFF);
            addReply(c,shared.ok);
        } else if (!strcasecmp((const char*)ptrFromObj(c->argv[2]),"off")) {
            c->flags |= CLIENT_REPLY_OFF;
        } else if (!strcasecmp((const char*)ptrFromObj(c->argv[2]),"skip")) {
            if (!(c->flags & CLIENT_REPLY_OFF))
                c->flags |= CLIENT_REPLY_SKIP_NEXT;
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
    } else if (!strcasecmp((const char*)ptrFromObj(c->argv[1]),"kill")) {
        /* CLIENT KILL <ip:port>
         * CLIENT KILL <option> [value] ... <option> [value] */
        char *addr = NULL;
        int type = -1;
        uint64_t id = 0;
        int skipme = 1;
        int killed = 0, close_this_client = 0;

        if (c->argc == 3) {
            /* Old style syntax: CLIENT KILL <addr> */
            addr = (char*)ptrFromObj(c->argv[2]);
            skipme = 0; /* With the old form, you can kill yourself. */
        } else if (c->argc > 3) {
            int i = 2; /* Next option index. */

            /* New style syntax: parse options. */
            while(i < c->argc) {
                int moreargs = c->argc > i+1;

                if (!strcasecmp((const char*)ptrFromObj(c->argv[i]),"id") && moreargs) {
                    long long tmp;

                    if (getLongLongFromObjectOrReply(c,c->argv[i+1],&tmp,NULL)
                        != C_OK) return;
                    id = tmp;
                } else if (!strcasecmp((const char*)ptrFromObj(c->argv[i]),"type") && moreargs) {
                    type = getClientTypeByName((const char*)ptrFromObj(c->argv[i+1]));
                    if (type == -1) {
                        addReplyErrorFormat(c,"Unknown client type '%s'",
                            (char*) ptrFromObj(c->argv[i+1]));
                        return;
                    }
                } else if (!strcasecmp((const char*)ptrFromObj(c->argv[i]),"addr") && moreargs) {
                    addr = (char*)ptrFromObj(c->argv[i+1]);
                } else if (!strcasecmp((const char*)ptrFromObj(c->argv[i]),"skipme") && moreargs) {
                    if (!strcasecmp((const char*)ptrFromObj(c->argv[i+1]),"yes")) {
                        skipme = 1;
                    } else if (!strcasecmp((const char*)ptrFromObj(c->argv[i+1]),"no")) {
                        skipme = 0;
                    } else {
                        addReply(c,shared.syntaxerr);
                        return;
                    }
                } else {
                    addReply(c,shared.syntaxerr);
                    return;
                }
                i += 2;
            }
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }

        /* Iterate clients killing all the matching clients. */
        listRewind(g_pserver->clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            client = (struct client*)listNodeValue(ln);
            if (addr && strcmp(getClientPeerId(client),addr) != 0) continue;
            if (type != -1 && getClientType(client) != type) continue;
            if (id != 0 && client->id != id) continue;
            if (c == client && skipme) continue;

            /* Kill it. */
            if (c == client) {
                close_this_client = 1;
            } else {
                if (FCorrectThread(client))
                    freeClient(client);
                else
                    freeClientAsync(client);
            }
            killed++;
        }

        /* Reply according to old/new format. */
        if (c->argc == 3) {
            if (killed == 0)
                addReplyError(c,"No such client");
            else
                addReply(c,shared.ok);
        } else {
            addReplyLongLong(c,killed);
        }

        /* If this client has to be closed, flag it as CLOSE_AFTER_REPLY
         * only after we queued the reply to its output buffers. */
        if (close_this_client) c->flags |= CLIENT_CLOSE_AFTER_REPLY;
    } else if (!strcasecmp((const char*)ptrFromObj(c->argv[1]),"unblock") && (c->argc == 3 ||
                                                          c->argc == 4))
    {
        /* CLIENT UNBLOCK <id> [timeout|error] */
        long long id;
        int unblock_error = 0;

        if (c->argc == 4) {
            if (!strcasecmp((const char*)ptrFromObj(c->argv[3]),"timeout")) {
                unblock_error = 0;
            } else if (!strcasecmp((const char*)ptrFromObj(c->argv[3]),"error")) {
                unblock_error = 1;
            } else {
                addReplyError(c,
                    "CLIENT UNBLOCK reason should be TIMEOUT or ERROR");
                return;
            }
        }
        if (getLongLongFromObjectOrReply(c,c->argv[2],&id,NULL)
            != C_OK) return;
        struct client *target = lookupClientByID(id);
        if (target && target->flags & CLIENT_BLOCKED) {
            if (unblock_error)
                addReplyError(target,
                    "-UNBLOCKED client unblocked via CLIENT UNBLOCK");
            else
                replyToBlockedClientTimedOut(target);
            unblockClient(target);
            addReply(c,shared.cone);
        } else {
            addReply(c,shared.czero);
        }
    } else if (!strcasecmp(szFromObj(c->argv[1]),"setname") && c->argc == 3) {
        /* CLIENT SETNAME */
        if (clientSetNameOrReply(c,c->argv[2]) == C_OK)
            addReply(c,shared.ok);
    } else if (!strcasecmp(szFromObj(c->argv[1]),"getname") && c->argc == 2) {
        /* CLIENT GETNAME */
        if (c->name)
            addReplyBulk(c,c->name);
        else
            addReplyNull(c, shared.nullbulk);
    } else if (!strcasecmp(szFromObj(c->argv[1]),"pause") && c->argc == 3) {
        /* CLIENT PAUSE */
        long long duration;

        if (getTimeoutFromObjectOrReply(c,c->argv[2],&duration,
                UNIT_MILLISECONDS) != C_OK) return;
        pauseClients(duration);
        addReply(c,shared.ok);
    } else if (!strcasecmp(szFromObj(c->argv[1]),"tracking") &&
               (c->argc == 3 || c->argc == 5))
    {
        /* CLIENT TRACKING (on|off) [REDIRECT <id>] */
        long long redir = 0;

        /* Parse the redirection option: we'll require the client with
         * the specified ID to exist right now, even if it is possible
         * it will get disconnected later. */
        if (c->argc == 5) {
            if (strcasecmp(szFromObj(c->argv[3]),"redirect") != 0) {
                addReply(c,shared.syntaxerr);
                return;
            } else {
                if (getLongLongFromObjectOrReply(c,c->argv[4],&redir,NULL) !=
                    C_OK) return;
                if (lookupClientByID(redir) == NULL) {
                    addReplyError(c,"The client ID you want redirect to "
                                    "does not exist");
                    return;
                }
            }
        }

        if (!strcasecmp(szFromObj(c->argv[2]),"on")) {
            enableTracking(c,redir);
        } else if (!strcasecmp(szFromObj(c->argv[2]),"off")) {
            disableTracking(c);
        } else {
            addReply(c,shared.syntaxerr);
            return;
        }
        addReply(c,shared.ok);
    } else {
        addReplyErrorFormat(c, "Unknown subcommand or wrong number of arguments for '%s'. Try CLIENT HELP", (char*)ptrFromObj(c->argv[1]));
    }
}

/* HELLO <protocol-version> [AUTH <user> <password>] [SETNAME <name>] */
void helloCommand(client *c) {
    long long ver;

    if (getLongLongFromObject(c->argv[1],&ver) != C_OK ||
        ver < 2 || ver > 3)
    {
        addReplyError(c,"-NOPROTO unsupported protocol version");
        return;
    }

    for (int j = 2; j < c->argc; j++) {
        int moreargs = (c->argc-1) - j;
        const char *opt = (const char*)ptrFromObj(c->argv[j]);
        if (!strcasecmp(opt,"AUTH") && moreargs >= 2) {
            if (ACLAuthenticateUser(c, c->argv[j+1], c->argv[j+2]) == C_ERR) {
                addReplyError(c,"-WRONGPASS invalid username-password pair");
                return;
            }
            j += 2;
        } else if (!strcasecmp(opt,"SETNAME") && moreargs) {
            if (clientSetNameOrReply(c, c->argv[j+1]) == C_ERR) return;
            j++;
        } else {
            addReplyErrorFormat(c,"Syntax error in HELLO option '%s'",opt);
            return;
        }
    }

    /* At this point we need to be authenticated to continue. */
    if (!c->authenticated) {
        addReplyError(c,"-NOAUTH HELLO must be called with the client already "
                        "authenticated, otherwise the HELLO AUTH <user> <pass> "
                        "option can be used to authenticate the client and "
                        "select the RESP protocol version at the same time");
        return;
    }

    /* Let's switch to the specified RESP mode. */
    c->resp = ver;
    addReplyMapLen(c,7);

    addReplyBulkCString(c,"server");
    addReplyBulkCString(c,"redis");

    addReplyBulkCString(c,"version");
    addReplyBulkCString(c,KEYDB_SET_VERSION);

    addReplyBulkCString(c,"proto");
    addReplyLongLong(c,3);

    addReplyBulkCString(c,"id");
    addReplyLongLong(c,c->id);

    addReplyBulkCString(c,"mode");
    if (g_pserver->sentinel_mode) addReplyBulkCString(c,"sentinel");
    if (g_pserver->cluster_enabled) addReplyBulkCString(c,"cluster");
    else addReplyBulkCString(c,"standalone");

    if (!g_pserver->sentinel_mode) {
        addReplyBulkCString(c,"role");
        addReplyBulkCString(c,listLength(g_pserver->masters) ? 
            g_pserver->fActiveReplica ? "active-replica" : "replica" 
            : "master");
    }

    addReplyBulkCString(c,"modules");
    addReplyLoadedModules(c);
}

/* This callback is bound to POST and "Host:" command names. Those are not
 * really commands, but are used in security attacks in order to talk to
 * Redis instances via HTTP, with a technique called "cross protocol scripting"
 * which exploits the fact that services like Redis will discard invalid
 * HTTP headers and will process what follows.
 *
 * As a protection against this attack, Redis will terminate the connection
 * when a POST or "Host:" header is seen, and will log the event from
 * time to time (to avoid creating a DOS as a result of too many logs). */
void securityWarningCommand(client *c) {
    static time_t logged_time;
    time_t now = time(NULL);

    if (labs(now-logged_time) > 60) {
        serverLog(LL_WARNING,"Possible SECURITY ATTACK detected. It looks like somebody is sending POST or Host: commands to Redis. This is likely due to an attacker attempting to use Cross Protocol Scripting to compromise your Redis instance. Connection aborted.");
        logged_time = now;
    }
    freeClientAsync(c);
}

/* Rewrite the command vector of the client. All the new objects ref count
 * is incremented. The old command vector is freed, and the old objects
 * ref count is decremented. */
void rewriteClientCommandVector(client *c, int argc, ...) {
    va_list ap;
    int j;
    robj **argv; /* The new argument vector */

    argv = (robj**)zmalloc(sizeof(robj*)*argc, MALLOC_LOCAL);
    va_start(ap,argc);
    for (j = 0; j < argc; j++) {
        robj *a;

        a = va_arg(ap, robj*);
        argv[j] = a;
        incrRefCount(a);
    }
    /* We free the objects in the original vector at the end, so we are
     * sure that if the same objects are reused in the new vector the
     * refcount gets incremented before it gets decremented. */
    for (j = 0; j < c->argc; j++) decrRefCount(c->argv[j]);
    zfree(c->argv);
    /* Replace argv and argc with our new versions. */
    c->argv = argv;
    c->argc = argc;
    c->cmd = lookupCommandOrOriginal((sds)ptrFromObj(c->argv[0]));
    serverAssertWithInfo(c,NULL,c->cmd != NULL);
    va_end(ap);
}

/* Completely replace the client command vector with the provided one. */
void replaceClientCommandVector(client *c, int argc, robj **argv) {
    freeClientArgv(c);
    zfree(c->argv);
    c->argv = argv;
    c->argc = argc;
    c->cmd = lookupCommandOrOriginal((sds)ptrFromObj(c->argv[0]));
    serverAssertWithInfo(c,NULL,c->cmd != NULL);
}

/* Rewrite a single item in the command vector.
 * The new val ref count is incremented, and the old decremented.
 *
 * It is possible to specify an argument over the current size of the
 * argument vector: in this case the array of objects gets reallocated
 * and c->argc set to the max value. However it's up to the caller to
 *
 * 1. Make sure there are no "holes" and all the arguments are set.
 * 2. If the original argument vector was longer than the one we
 *    want to end with, it's up to the caller to set c->argc and
 *    free the no longer used objects on c->argv. */
void rewriteClientCommandArgument(client *c, int i, robj *newval) {
    robj *oldval;

    if (i >= c->argc) {
        c->argv = (robj**)zrealloc(c->argv,sizeof(robj*)*(i+1), MALLOC_LOCAL);
        c->argc = i+1;
        c->argv[i] = NULL;
    }
    oldval = c->argv[i];
    c->argv[i] = newval;
    incrRefCount(newval);
    if (oldval) decrRefCount(oldval);

    /* If this is the command name make sure to fix c->cmd. */
    if (i == 0) {
        c->cmd = lookupCommandOrOriginal((sds)ptrFromObj(c->argv[0]));
        serverAssertWithInfo(c,NULL,c->cmd != NULL);
    }
}

/* This function returns the number of bytes that Redis is
 * using to store the reply still not read by the client.
 *
 * Note: this function is very fast so can be called as many time as
 * the caller wishes. The main usage of this function currently is
 * enforcing the client output length limits. */
unsigned long getClientOutputBufferMemoryUsage(client *c) {
    unsigned long list_item_size = sizeof(listNode) + sizeof(clientReplyBlock);
    return c->reply_bytes + (list_item_size*listLength(c->reply)) + c->buflenAsync;
}

/* Get the class of a client, used in order to enforce limits to different
 * classes of clients.
 *
 * The function will return one of the following:
 * CLIENT_TYPE_NORMAL -> Normal client
 * CLIENT_TYPE_SLAVE  -> Slave or client executing MONITOR command
 * CLIENT_TYPE_PUBSUB -> Client subscribed to Pub/Sub channels
 * CLIENT_TYPE_MASTER -> The client representing our replication master.
 */
int getClientType(client *c) {
    if (c->flags & CLIENT_MASTER) return CLIENT_TYPE_MASTER;
    if ((c->flags & CLIENT_SLAVE) && !(c->flags & CLIENT_MONITOR))
        return CLIENT_TYPE_SLAVE;
    if (c->flags & CLIENT_PUBSUB) return CLIENT_TYPE_PUBSUB;
    return CLIENT_TYPE_NORMAL;
}

int getClientTypeByName(const char *name) {
    if (!strcasecmp(name,"normal")) return CLIENT_TYPE_NORMAL;
    else if (!strcasecmp(name,"slave")) return CLIENT_TYPE_SLAVE;
    else if (!strcasecmp(name,"replica")) return CLIENT_TYPE_SLAVE;
    else if (!strcasecmp(name,"pubsub")) return CLIENT_TYPE_PUBSUB;
    else if (!strcasecmp(name,"master")) return CLIENT_TYPE_MASTER;
    else return -1;
}

const char *getClientTypeName(int clientType) {
    switch(clientType) {
    case CLIENT_TYPE_NORMAL: return "normal";
    case CLIENT_TYPE_SLAVE:  return "slave";
    case CLIENT_TYPE_PUBSUB: return "pubsub";
    case CLIENT_TYPE_MASTER: return "master";
    default:                       return NULL;
    }
}

/* The function checks if the client reached output buffer soft or hard
 * limit, and also update the state needed to check the soft limit as
 * a side effect.
 *
 * Return value: non-zero if the client reached the soft or the hard limit.
 *               Otherwise zero is returned. */
int checkClientOutputBufferLimits(client *c) {
    int soft = 0, hard = 0;
    unsigned long used_mem = getClientOutputBufferMemoryUsage(c);

    int clientType = getClientType(c);
    /* For the purpose of output buffer limiting, masters are handled
     * like normal clients. */
    if (clientType == CLIENT_TYPE_MASTER) clientType = CLIENT_TYPE_NORMAL;

    if (cserver.client_obuf_limits[clientType].hard_limit_bytes &&
        used_mem >= cserver.client_obuf_limits[clientType].hard_limit_bytes)
        hard = 1;
    if (cserver.client_obuf_limits[clientType].soft_limit_bytes &&
        used_mem >= cserver.client_obuf_limits[clientType].soft_limit_bytes)
        soft = 1;

    /* We need to check if the soft limit is reached continuously for the
     * specified amount of seconds. */
    if (soft) {
        if (c->obuf_soft_limit_reached_time == 0) {
            c->obuf_soft_limit_reached_time = g_pserver->unixtime;
            soft = 0; /* First time we see the soft limit reached */
        } else {
            time_t elapsed = g_pserver->unixtime - c->obuf_soft_limit_reached_time;

            if (elapsed <=
                cserver.client_obuf_limits[clientType].soft_limit_seconds) {
                soft = 0; /* The client still did not reached the max number of
                             seconds for the soft limit to be considered
                             reached. */
            }
        }
    } else {
        c->obuf_soft_limit_reached_time = 0;
    }
    return soft || hard;
}

/* Asynchronously close a client if soft or hard limit is reached on the
 * output buffer size. The caller can check if the client will be closed
 * checking if the client CLIENT_CLOSE_ASAP flag is set.
 *
 * Note: we need to close the client asynchronously because this function is
 * called from contexts where the client can't be freed safely, i.e. from the
 * lower level functions pushing data inside the client output buffers. */
void asyncCloseClientOnOutputBufferLimitReached(client *c) {
    if (c->fd == -1) return; /* It is unsafe to free fake clients. */
    serverAssert(c->reply_bytes < SIZE_MAX-(1024*64));
    if (c->reply_bytes == 0 || c->flags & CLIENT_CLOSE_ASAP) return;
    if (checkClientOutputBufferLimits(c)) {
        sds client = catClientInfoString(sdsempty(),c);

        freeClientAsync(c);
        serverLog(LL_WARNING,"Client %s scheduled to be closed ASAP for overcoming of output buffer limits.", client);
        sdsfree(client);
    }
}

/* Helper function used by freeMemoryIfNeeded() in order to flush slaves
 * output buffers without returning control to the event loop.
 * This is also called by SHUTDOWN for a best-effort attempt to send
 * slaves the latest writes. */
void flushSlavesOutputBuffers(void) {
    serverAssert(GlobalLocksAcquired());
    listIter li;
    listNode *ln;

    listRewind(g_pserver->slaves,&li);
    while((ln = listNext(&li))) {
        client *replica = (client*)listNodeValue(ln);
        int events;

        if (!FCorrectThread(replica))
            continue;   // we cannot synchronously flush other thread's clients

        /* Note that the following will not flush output buffers of slaves
         * in STATE_ONLINE but having put_online_on_ack set to true: in this
         * case the writable event is never installed, since the purpose
         * of put_online_on_ack is to postpone the moment it is installed.
         * This is what we want since slaves in this state should not receive
         * writes before the first ACK. */
        events = aeGetFileEvents(g_pserver->rgthreadvar[replica->iel].el,replica->fd);
        if (events & AE_WRITABLE &&
            replica->replstate == SLAVE_STATE_ONLINE &&
            clientHasPendingReplies(replica))
        {
            writeToClient(replica->fd,replica,0);
        }
    }
}

/* Pause clients up to the specified unixtime (in ms). While clients
 * are paused no command is processed from clients, so the data set can't
 * change during that time.
 *
 * However while this function pauses normal and Pub/Sub clients, slaves are
 * still served, so this function can be used on server upgrades where it is
 * required that slaves process the latest bytes from the replication stream
 * before being turned to masters.
 *
 * This function is also internally used by Redis Cluster for the manual
 * failover procedure implemented by CLUSTER FAILOVER.
 *
 * The function always succeed, even if there is already a pause in progress.
 * In such a case, the pause is extended if the duration is more than the
 * time left for the previous duration. However if the duration is smaller
 * than the time left for the previous pause, no change is made to the
 * left duration. */
void pauseClients(mstime_t end) {
    if (!g_pserver->clients_paused || end > g_pserver->clients_pause_end_time)
        g_pserver->clients_pause_end_time = end;
    g_pserver->clients_paused = 1;
}

/* Return non-zero if clients are currently paused. As a side effect the
 * function checks if the pause time was reached and clear it. */
int clientsArePaused(void) {
    if (g_pserver->clients_paused &&
        g_pserver->clients_pause_end_time < g_pserver->mstime)
    {
        aeAcquireLock();
        listNode *ln;
        listIter li;
        client *c;

        g_pserver->clients_paused = 0;

        /* Put all the clients in the unblocked clients queue in order to
         * force the re-processing of the input buffer if any. */
        listRewind(g_pserver->clients,&li);
        while ((ln = listNext(&li)) != NULL) {
            c = (client*)listNodeValue(ln);

            /* Don't touch slaves and blocked clients.
             * The latter pending requests will be processed when unblocked. */
            if (c->flags & (CLIENT_SLAVE|CLIENT_BLOCKED)) continue;
            queueClientForReprocessing(c);
        }
        aeReleaseLock();
    }
    return g_pserver->clients_paused;
}

/* This function is called by Redis in order to process a few events from
 * time to time while blocked into some not interruptible operation.
 * This allows to reply to clients with the -LOADING error while loading the
 * data set at startup or after a full resynchronization with the master
 * and so forth.
 *
 * It calls the event loop in order to process a few events. Specifically we
 * try to call the event loop 4 times as long as we receive acknowledge that
 * some event was processed, in order to go forward with the accept, read,
 * write, close sequence needed to serve a client.
 *
 * The function returns the total number of events processed. */
int processEventsWhileBlocked(int iel) {
    int iterations = 4; /* See the function top-comment. */
    int count = 0;

    aeReleaseLock();
    while (iterations--) {
        int events = 0;
        events += aeProcessEvents(g_pserver->rgthreadvar[iel].el, AE_FILE_EVENTS|AE_DONT_WAIT);
        events += handleClientsWithPendingWrites(iel);
        if (!events) break;
        count += events;
    }
    aeAcquireLock();
    return count;
}

