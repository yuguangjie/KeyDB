/* 
 * Copyright (c) 2019, John Sully <john at eqalpha dot com>
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

#include "fastlock.h"
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sched.h>
#include <atomic>
#include <assert.h>
#include <pthread.h>
#include <limits.h>
#include <map>
#ifdef __linux__
#include <linux/futex.h>
#endif
#include <string.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#ifdef TARGET_OS_MAC
/* The CLANG that ships with Mac OS doesn't have these builtins.
    but on x86 they are just normal reads/writes anyways */
#define __atomic_load_4(ptr, csq) (*(reinterpret_cast<const volatile uint32_t*>(ptr)))
#define __atomic_load_2(ptr, csq) (*(reinterpret_cast<const volatile uint16_t*>(ptr)))

#define __atomic_store_4(ptr, val, csq) (*(reinterpret_cast<volatile uint32_t*>(ptr)) = val)
#endif
#endif

#ifndef UNUSED
#define UNUSED(x) ((void)x)
#endif


/****************************************************
 *
 *      Implementation of a fair spinlock.  To promote fairness we
 *      use a ticket lock instead of a raw spinlock
 * 
 ****************************************************/


#if !defined(__has_feature)
    #define __has_feature(x) 0
#endif

#if __has_feature(thread_sanitizer)

    /* Report that a lock has been created at address "lock". */
    #define ANNOTATE_RWLOCK_CREATE(lock) \
        AnnotateRWLockCreate(__FILE__, __LINE__, lock)

    /* Report that the lock at address "lock" is about to be destroyed. */
    #define ANNOTATE_RWLOCK_DESTROY(lock) \
        AnnotateRWLockDestroy(__FILE__, __LINE__, lock)

    /* Report that the lock at address "lock" has been acquired.
       is_w=1 for writer lock, is_w=0 for reader lock. */
    #define ANNOTATE_RWLOCK_ACQUIRED(lock, is_w) \
        AnnotateRWLockAcquired(__FILE__, __LINE__, lock, is_w)

    /* Report that the lock at address "lock" is about to be released. */
    #define ANNOTATE_RWLOCK_RELEASED(lock, is_w) \
      AnnotateRWLockReleased(__FILE__, __LINE__, lock, is_w)

    #if defined(DYNAMIC_ANNOTATIONS_WANT_ATTRIBUTE_WEAK)
        #if defined(__GNUC__)
            #define DYNAMIC_ANNOTATIONS_ATTRIBUTE_WEAK __attribute__((weak))
        #else
            /* TODO(glider): for Windows support we may want to change this macro in order
               to prepend __declspec(selectany) to the annotations' declarations. */
            #error weak annotations are not supported for your compiler
        #endif
    #else
        #define DYNAMIC_ANNOTATIONS_ATTRIBUTE_WEAK
    #endif

    extern "C" {
    void AnnotateRWLockCreate(
        const char *file, int line,
        const volatile void *lock) DYNAMIC_ANNOTATIONS_ATTRIBUTE_WEAK;
    void AnnotateRWLockDestroy(
        const char *file, int line,
        const volatile void *lock) DYNAMIC_ANNOTATIONS_ATTRIBUTE_WEAK;
    void AnnotateRWLockAcquired(
        const char *file, int line,
        const volatile void *lock, long is_w) DYNAMIC_ANNOTATIONS_ATTRIBUTE_WEAK;
    void AnnotateRWLockReleased(
        const char *file, int line,
        const volatile void *lock, long is_w) DYNAMIC_ANNOTATIONS_ATTRIBUTE_WEAK;
    }

#else

    #define ANNOTATE_RWLOCK_CREATE(lock)
    #define ANNOTATE_RWLOCK_DESTROY(lock)
    #define ANNOTATE_RWLOCK_ACQUIRED(lock, is_w)
    #define ANNOTATE_RWLOCK_RELEASED(lock, is_w)

#endif

#pragma weak _serverPanic
extern "C" void _serverPanic(const char * /*file*/, int /*line*/, const char * /*msg*/, ...)
{
    *((char*)-1) = 'x';
}

class DeadlockDetector
{
    std::map<pid_t, fastlock *> m_mapwait;
    fastlock m_lock;
public:
    void registerwait(fastlock *lock, pid_t thispid)
    {
        if (lock == &m_lock)
            return;
        fastlock_lock(&m_lock);
        m_mapwait.insert(std::make_pair(thispid, lock));

        // Detect cycles
        pid_t pidCheck = thispid;
        for (;;)
        {
            auto itr = m_mapwait.find(pidCheck);
            if (itr == m_mapwait.end())
                break;
            pidCheck = itr->second->m_pidOwner;
            if (pidCheck == thispid)
                _serverPanic(__FILE__, __LINE__, "Deadlock detected");
        }
        fastlock_unlock(&m_lock);
    }

    void clearwait(fastlock *lock, pid_t thispid)
    {
        if (lock == &m_lock)
            return;
        fastlock_lock(&m_lock);
        m_mapwait.erase(thispid);
        fastlock_unlock(&m_lock);
    }
};

DeadlockDetector g_dlock;

extern "C" void registerwait(fastlock *lock, pid_t thispid)
{
    g_dlock.registerwait(lock, thispid);
}

extern "C" void clearwait(fastlock *lock, pid_t thispid)
{
    g_dlock.clearwait(lock, thispid);
}

static_assert(sizeof(pid_t) <= sizeof(fastlock::m_pidOwner), "fastlock::m_pidOwner not large enough");
uint64_t g_longwaits = 0;

uint64_t fastlock_getlongwaitcount()
{
    uint64_t rval;
    __atomic_load(&g_longwaits, &rval, __ATOMIC_RELAXED);
    return rval;
}

#ifndef ASM_SPINLOCK
#ifdef __linux__
static int futex(volatile unsigned *uaddr, int futex_op, int val,
    const struct timespec *timeout, int val3)
{
    return syscall(SYS_futex, uaddr, futex_op, val,
                    timeout, uaddr, val3);
}
#endif
#endif

extern "C" pid_t gettid()
{
    static thread_local int pidCache = -1;
#ifdef __linux__
    if (pidCache == -1)
        pidCache = syscall(SYS_gettid);
#else
	if (pidCache == -1) {
		uint64_t tidT;
		pthread_threadid_np(nullptr, &tidT);
		assert(tidT < UINT_MAX);
		pidCache = (int)tidT;
	}
#endif
    return pidCache;
}

extern "C" void fastlock_init(struct fastlock *lock)
{
    lock->m_ticket.m_active = 0;
    lock->m_ticket.m_avail = 0;
    lock->m_depth = 0;
    lock->m_pidOwner = -1;
    lock->futex = 0;
    ANNOTATE_RWLOCK_CREATE(lock);
}

#ifndef ASM_SPINLOCK
extern "C" void fastlock_lock(struct fastlock *lock)
{
    int pidOwner;
    __atomic_load(&lock->m_pidOwner, &pidOwner, __ATOMIC_ACQUIRE);
    if (pidOwner == gettid())
    {
        ++lock->m_depth;
        return;
    }

    int tid = gettid();
    unsigned myticket = __atomic_fetch_add(&lock->m_ticket.m_avail, 1, __ATOMIC_RELEASE);
#ifdef __linux__
    unsigned mask = (1U << (myticket % 32));
#endif
    int cloops = 0;
    ticket ticketT;

    __atomic_load(&lock->m_ticket.u, &ticketT.u, __ATOMIC_ACQUIRE);
    if ((ticketT.u & 0xffff) != myticket)
    {
        registerwait(lock, tid);
        for (;;)
        {
            __atomic_load(&lock->m_ticket.u, &ticketT.u, __ATOMIC_ACQUIRE);
            if ((ticketT.u & 0xffff) == myticket)
                break;

#if defined(__i386__) || defined(__amd64__)
            __asm__ ("pause");
#endif
            if ((++cloops % 1024*1024) == 0)
            {
#ifdef __linux__
                __atomic_fetch_or(&lock->futex, mask, __ATOMIC_ACQUIRE);
                futex(&lock->m_ticket.u, FUTEX_WAIT_BITSET_PRIVATE, ticketT.u, nullptr, mask);
                __atomic_fetch_and(&lock->futex, ~mask, __ATOMIC_RELEASE);
#endif
                __atomic_fetch_add(&g_longwaits, 1, __ATOMIC_RELAXED);
            }
        }
        clearwait(lock, tid);
    }

    lock->m_depth = 1;
    __atomic_store(&lock->m_pidOwner, &tid, __ATOMIC_RELEASE);
    ANNOTATE_RWLOCK_ACQUIRED(lock, true);
    std::atomic_thread_fence(std::memory_order_acquire);
}

extern "C" int fastlock_trylock(struct fastlock *lock, int fWeak)
{
    int tid;
    __atomic_load(&lock->m_pidOwner, &tid, __ATOMIC_ACQUIRE);
    if (tid == gettid())
    {
        ++lock->m_depth;
        return true;
    }

    // cheap test
    struct ticket ticketT;
    __atomic_load(&lock->m_ticket.u, &ticketT.u, __ATOMIC_ACQUIRE);
    if (ticketT.m_active != ticketT.m_avail)
        return false;

    uint16_t active = ticketT.m_active;
    uint16_t next = active + 1;

    struct ticket ticket_expect { { { active, active } } };
    struct ticket ticket_setiflocked { { { active, next } } };
    if (__atomic_compare_exchange(&lock->m_ticket, &ticket_expect, &ticket_setiflocked, fWeak /*weak*/, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    {
        lock->m_depth = 1;
        tid = gettid();
        __atomic_store(&lock->m_pidOwner, &tid,  __ATOMIC_RELEASE);
        ANNOTATE_RWLOCK_ACQUIRED(lock, true);
        return true;
    }
    return false;
}

#ifdef __linux__
#define ROL32(v, shift) ((v << shift) | (v >> (32-shift)))
void unlock_futex(struct fastlock *lock, uint16_t ifutex)
{
    unsigned mask = (1U << (ifutex % 32));
    unsigned futexT;
    __atomic_load(&lock->futex, &futexT, __ATOMIC_RELAXED);
    futexT &= mask;
    
    if (futexT == 0)
        return;
    
    for (;;)
    {
        __atomic_load(&lock->futex, &futexT, __ATOMIC_ACQUIRE);
        futexT &= mask;
        if (!futexT)
            break;

        if (futex(&lock->m_ticket.u, FUTEX_WAKE_BITSET_PRIVATE, INT_MAX, nullptr, mask) == 1)
            break;
    }
}
#endif

extern "C" void fastlock_unlock(struct fastlock *lock)
{
    --lock->m_depth;
    if (lock->m_depth == 0)
    {
        int pidT;
        __atomic_load(&lock->m_pidOwner, &pidT, __ATOMIC_RELAXED);
        assert(pidT >= 0);  // unlock after free
        int t = -1;
        __atomic_store(&lock->m_pidOwner, &t, __ATOMIC_RELEASE);
        std::atomic_thread_fence(std::memory_order_release);
        ANNOTATE_RWLOCK_RELEASED(lock, true);
        uint16_t activeNew = __atomic_add_fetch(&lock->m_ticket.m_active, 1, __ATOMIC_RELEASE);  // on x86 the atomic is not required here, but ASM handles that case
#ifdef __linux__
        unlock_futex(lock, activeNew);
#else
		UNUSED(activeNew);
#endif
    }
}
#endif

extern "C" void fastlock_free(struct fastlock *lock)
{
    // NOP
    assert((lock->m_ticket.m_active == lock->m_ticket.m_avail)                                        // Asser the lock is unlocked
        || (lock->m_pidOwner == gettid() && (lock->m_ticket.m_active == lock->m_ticket.m_avail-1)));  // OR we own the lock and nobody else is waiting
    lock->m_pidOwner = -2;  // sentinal value indicating free
    ANNOTATE_RWLOCK_DESTROY(lock);
}


bool fastlock::fOwnLock()
{
    int tid;
    __atomic_load(&m_pidOwner, &tid, __ATOMIC_RELAXED);
    return gettid() == tid;
}

int fastlock_unlock_recursive(struct fastlock *lock)
{
    int rval = lock->m_depth;
    lock->m_depth = 1;
    fastlock_unlock(lock);
    return rval;
}

void fastlock_lock_recursive(struct fastlock *lock, int nesting)
{
    fastlock_lock(lock);
    lock->m_depth = nesting;
}