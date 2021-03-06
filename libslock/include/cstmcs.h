/*
 * File: cstmcs.h
 * Author: Sanidhya Kashyap <sanidhya@gatech.edu>
 *         Changwoo Min <changwoo@gatech.edu>
 *
 * Description:
 *      Implementation of an CSTMCS lock
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2017 Sanidhya Kashyap, Changwoo Min
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */




#ifndef _CSTMCS_H_
#define _CSTMCS_H_

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#ifndef __sparc__
#include <numa.h>
#endif
#include <pthread.h>
#include "utils.h"
#include "atomic_ops.h"

//#define CST_DEBUG
#define ____cacheline_aligned  __attribute__ ((aligned (CACHE_LINE_SIZE)))

/**
 * Timestamp + cpu and numa node info that we can get with rdtscp()
 */
struct nid_clock_info {
    uint32_t nid;
    uint64_t timestamp;
#ifdef CST_DEBUG
	int32_t cid;
#endif
};

/**
 * linux-like circular list manipulation
 */
struct list_head {
	volatile struct list_head *next;
};


/**
 * mutex structure
 */
/* associated spin time during the traversal */
#define DEFAULT_SPIN_TIME          (1 << 15) /* max cost to put back to rq */
#define DEFAULT_HOLDER_SPIN_TIME   (DEFAULT_SPIN_TIME)
#define DEFAULT_WAITER_SPIN_TIME   (DEFAULT_SPIN_TIME)
#define DEFAULT_WAITER_WAKEUP_TIME (DEFAULT_SPIN_TIME)
/* This is an approximation */
#define CYCLES_TO_COUNTERS(v)      (v / (1U << 4))

#define NUMA_GID_BITS               (4)  /* even = empty, odd = not empty */
#define NUMA_GID_SHIFT(_n)          ((_n) * NUMA_GID_BITS)
#define NUMA_MAX_DOMAINS            (64 / NUMA_GEN_ID_BITS)
#define NUMA_NID_MASK(_n)           ((0xF) << NUMA_GID_SHIFT(_n))
#define NUMA_GID_MASK(_n, _g)       (((_g) & (0xF)) << NUMA_GID_SHIFT(_n))
#define numa_gid_inc(_gid)          (((_gid) & ~0x1) + 2)
#define numa_gid_not_empty(_gid)    ((_gid) & 0x1)

#define NUMA_BATCH_SIZE             (128) /* per numa throughput */
#define NUMA_WAITING_SPINNERS       (4) /* spinners in the waiter spin phase */

/* lock status */
#define STATE_PARKED (0)
#define STATE_LOCKED (1)

/* this will be around 8 milliseconds which is huge!!! */
#define MAX_SPIN_THRESHOLD          (1U << 20)
/* this is the cost of a getpriority syscall. */
#define MIN_SPIN_THRESHOLD          (1U << 7)


typedef struct qnode {
    volatile uint64_t status;
    volatile struct qnode *next;
} cstmcs_qnode ____cacheline_aligned;

struct snode {
    /*
     * ONE CACHELINE
     * ticket info
     */
    struct qnode *qtail;
    /* batch count */
    int32_t num_proc; /* #batched processes */

    /*
     * ANOTHER CACHELINE
     * tail management
     */
    /* MCS tail to know who is the next waiter */
    struct snode *gnext ____cacheline_aligned;

    /*
     * ANOTHER CACHELINE
     * snode bookeeping for various uses
     */

    /* list node like Linux list */
    struct list_head numa_node ____cacheline_aligned;
    /* status update of the waiter */
    volatile int32_t status;
    /* node id */
    int32_t nid; /* alive: > 0 | zombie: < 0 */
    /* epoch to be used for the zombie */
    uint32_t epoch;

    /* other bookeeping stuff */
    uint32_t holder_spin_time;
    uint32_t waiter_spin_time;
    uint32_t waiter_wakeup_time;
    uint32_t task_priority;

#ifdef CST_DEBUG
	int32_t 	cid;
#endif
} ____cacheline_aligned;

struct numa_head {
    struct list_head head;
};

typedef struct cst_t {
    /* snode which holds the hold */
    volatile struct snode *serving_socket;
    /* tail for the MCS style */
    volatile struct snode *gtail;
    /* Fancy way to allocate the snode */
    volatile uint64_t ngid_vec;

    /* Maintain the snode list that tells how many sockets are active */
    struct numa_head numa_list;
} cstmcs_mutex_t ____cacheline_aligned;


typedef volatile cstmcs_qnode *cstmcs_qnode_ptr;
typedef volatile cstmcs_mutex_t cstmcs_lock; //initialized to NULL

typedef cstmcs_qnode* cstmcs_local_params;

typedef struct cstmcs_global_params {
    cstmcs_mutex_t *the_lock;
} cstmcs_global_params;


/*
   Methods for easy lock array manipulation
   */

cstmcs_global_params* init_cstmcs_array_global(uint32_t num_locks);

cstmcs_qnode** init_cstmcs_array_local(uint32_t thread_num, uint32_t num_locks);

void end_cstmcs_array_local(cstmcs_qnode** the_qnodes, uint32_t size);

void end_cstmcs_array_global(cstmcs_global_params* the_locks, uint32_t size);
/*
   single lock manipulation
   */

int init_cstmcs_global(cstmcs_global_params* the_lock);

int init_cstmcs_local(uint32_t thread_num, cstmcs_qnode** the_qnode);

void end_cstmcs_local(cstmcs_qnode* the_qnodes);

void end_cstmcs_global(cstmcs_global_params the_locks);

/*
 *  Acquire and release methods
 */

void cstmcs_acquire(cstmcs_lock *the_lock, cstmcs_qnode_ptr I);

void cstmcs_release(cstmcs_lock *the_lock, cstmcs_qnode_ptr I);

int is_free_cstmcs(cstmcs_lock *L );

int cstmcs_trylock(cstmcs_lock *L, cstmcs_qnode_ptr I);
#endif
