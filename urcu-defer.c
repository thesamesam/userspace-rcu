/*
 * urcu-defer.c
 *
 * Userspace RCU library - batch memory reclamation
 *
 * Copyright (c) 2009 Mathieu Desnoyers <mathieu.desnoyers@polymtl.ca>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <stdio.h>
#include <pthread.h>
#include <signal.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <sys/time.h>
#include <syscall.h>
#include <unistd.h>

#include "urcu-defer-static.h"
/* Do not #define _LGPL_SOURCE to ensure we can emit the wrapper symbols */
#include "urcu-defer.h"

#define futex(...)	syscall(__NR_futex, __VA_ARGS__)
#define FUTEX_WAIT		0
#define FUTEX_WAKE		1

void __attribute__((destructor)) urcu_defer_exit(void);

extern void synchronize_rcu(void);

/*
 * urcu_defer_mutex nests inside defer_thread_mutex.
 */
static pthread_mutex_t urcu_defer_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t defer_thread_mutex = PTHREAD_MUTEX_INITIALIZER;

static int defer_thread_futex;

/*
 * Written to only by each individual deferer. Read by both the deferer and
 * the reclamation tread.
 */
static struct defer_queue __thread defer_queue;

/* Thread IDs of registered deferers */
#define INIT_NUM_THREADS 4

struct deferer_registry {
	pthread_t tid;
	struct defer_queue *defer_queue;
	unsigned long last_head;
};

static struct deferer_registry *registry;
static int num_deferers, alloc_deferers;

static pthread_t tid_defer;

static void internal_urcu_lock(pthread_mutex_t *mutex)
{
	int ret;

#ifndef DISTRUST_SIGNALS_EXTREME
	ret = pthread_mutex_lock(mutex);
	if (ret) {
		perror("Error in pthread mutex lock");
		exit(-1);
	}
#else /* #ifndef DISTRUST_SIGNALS_EXTREME */
	while ((ret = pthread_mutex_trylock(mutex)) != 0) {
		if (ret != EBUSY && ret != EINTR) {
			printf("ret = %d, errno = %d\n", ret, errno);
			perror("Error in pthread mutex lock");
			exit(-1);
		}
		pthread_testcancel();
		poll(NULL,0,10);
	}
#endif /* #else #ifndef DISTRUST_SIGNALS_EXTREME */
}

static void internal_urcu_unlock(pthread_mutex_t *mutex)
{
	int ret;

	ret = pthread_mutex_unlock(mutex);
	if (ret) {
		perror("Error in pthread mutex unlock");
		exit(-1);
	}
}

/*
 * Wake-up any waiting defer thread. Called from many concurrent threads.
 */
static void wake_up_defer(void)
{
	if (unlikely(atomic_read(&defer_thread_futex) == -1)) {
		atomic_set(&defer_thread_futex, 0);
		futex(&defer_thread_futex, FUTEX_WAKE, 1,
		      NULL, NULL, 0);
	}
}

static unsigned long rcu_defer_num_callbacks(void)
{
	unsigned long num_items = 0, head;
	struct deferer_registry *index;

	internal_urcu_lock(&urcu_defer_mutex);
	for (index = registry; index < registry + num_deferers; index++) {
		head = LOAD_SHARED(index->defer_queue->head);
		num_items += head - index->defer_queue->tail;
	}
	internal_urcu_unlock(&urcu_defer_mutex);
	return num_items;
}

/*
 * Defer thread waiting. Single thread.
 */
static void wait_defer(void)
{
	atomic_dec(&defer_thread_futex);
	smp_mb();	/* Write futex before read queue */
	if (rcu_defer_num_callbacks()) {
		smp_mb();	/* Read queue before write futex */
		/* Callbacks are queued, don't wait. */
		atomic_set(&defer_thread_futex, 0);
	} else {
		smp_rmb();	/* Read queue before read futex */
		if (atomic_read(&defer_thread_futex) == -1)
			futex(&defer_thread_futex, FUTEX_WAIT, -1,
			      NULL, NULL, 0);
	}
}

/*
 * Must be called after Q.S. is reached.
 */
static void rcu_defer_barrier_queue(struct defer_queue *queue,
				    unsigned long head)
{
	unsigned long i;
	void (*fct)(void *p);
	void *p;

	/*
	 * Tail is only modified when lock is held.
	 * Head is only modified by owner thread.
	 */

	for (i = queue->tail; i != head;) {
		smp_rmb();       /* read head before q[]. */
		p = LOAD_SHARED(queue->q[i++ & DEFER_QUEUE_MASK]);
		if (unlikely(DQ_IS_FCT_BIT(p))) {
			DQ_CLEAR_FCT_BIT(p);
			queue->last_fct_out = p;
			p = LOAD_SHARED(queue->q[i++ & DEFER_QUEUE_MASK]);
		} else if (unlikely(p == DQ_FCT_MARK)) {
			p = LOAD_SHARED(queue->q[i++ & DEFER_QUEUE_MASK]);
			queue->last_fct_out = p;
			p = LOAD_SHARED(queue->q[i++ & DEFER_QUEUE_MASK]);
		}
		fct = queue->last_fct_out;
		fct(p);
	}
	smp_mb();	/* push tail after having used q[] */
	STORE_SHARED(queue->tail, i);
}

static void _rcu_defer_barrier_thread(void)
{
	unsigned long head, num_items;

	head = defer_queue.head;
	num_items = head - defer_queue.tail;
	if (unlikely(!num_items))
		return;
	synchronize_rcu();
	rcu_defer_barrier_queue(&defer_queue, head);
}


void rcu_defer_barrier_thread(void)
{
	internal_urcu_lock(&urcu_defer_mutex);
	_rcu_defer_barrier_thread();
	internal_urcu_unlock(&urcu_defer_mutex);
}

/*
 * rcu_defer_barrier - Execute all queued rcu callbacks.
 *
 * Execute all RCU callbacks queued before rcu_defer_barrier() execution.
 * All callbacks queued on the local thread prior to a rcu_defer_barrier() call
 * are guaranteed to be executed.
 * Callbacks queued by other threads concurrently with rcu_defer_barrier()
 * execution are not guaranteed to be executed in the current batch (could
 * be left for the next batch). These callbacks queued by other threads are only
 * guaranteed to be executed if there is explicit synchronization between
 * the thread adding to the queue and the thread issuing the defer_barrier call.
 */

void rcu_defer_barrier(void)
{
	struct deferer_registry *index;
	unsigned long num_items = 0;

	if (!registry)
		return;

	internal_urcu_lock(&urcu_defer_mutex);
	for (index = registry; index < registry + num_deferers; index++) {
		index->last_head = LOAD_SHARED(index->defer_queue->head);
		num_items += index->last_head - index->defer_queue->tail;
	}
	if (likely(!num_items)) {
		/*
		 * We skip the grace period because there are no queued
		 * callbacks to execute.
		 */
		goto end;
	}
	synchronize_rcu();
	for (index = registry; index < registry + num_deferers; index++)
		rcu_defer_barrier_queue(index->defer_queue,
					  index->last_head);
end:
	internal_urcu_unlock(&urcu_defer_mutex);
}

/*
 * _rcu_defer_queue - Queue a RCU callback.
 */
void _rcu_defer_queue(void (*fct)(void *p), void *p)
{
	unsigned long head, tail;

	/*
	 * Head is only modified by ourself. Tail can be modified by reclamation
	 * thread.
	 */
	head = defer_queue.head;
	tail = LOAD_SHARED(defer_queue.tail);

	/*
	 * If queue is full, empty it ourself.
	 * Worse-case: must allow 2 supplementary entries for fct pointer.
	 */
	if (unlikely(head - tail >= DEFER_QUEUE_SIZE - 2)) {
		assert(head - tail <= DEFER_QUEUE_SIZE);
		rcu_defer_barrier_thread();
		assert(head - LOAD_SHARED(defer_queue.tail) == 0);
	}

	if (unlikely(defer_queue.last_fct_in != fct)) {
		defer_queue.last_fct_in = fct;
		if (unlikely(DQ_IS_FCT_BIT(fct) || fct == DQ_FCT_MARK)) {
			/*
			 * If the function to encode is not aligned or the
			 * marker, write DQ_FCT_MARK followed by the function
			 * pointer.
			 */
			_STORE_SHARED(defer_queue.q[head++ & DEFER_QUEUE_MASK],
				      DQ_FCT_MARK);
			_STORE_SHARED(defer_queue.q[head++ & DEFER_QUEUE_MASK],
				      fct);
		} else {
			DQ_SET_FCT_BIT(fct);
			_STORE_SHARED(defer_queue.q[head++ & DEFER_QUEUE_MASK],
				      fct);
		}
	} else {
		if (unlikely(DQ_IS_FCT_BIT(p) || p == DQ_FCT_MARK)) {
			/*
			 * If the data to encode is not aligned or the marker,
			 * write DQ_FCT_MARK followed by the function pointer.
			 */
			_STORE_SHARED(defer_queue.q[head++ & DEFER_QUEUE_MASK],
				      DQ_FCT_MARK);
			_STORE_SHARED(defer_queue.q[head++ & DEFER_QUEUE_MASK],
				      fct);
		}
	}
	_STORE_SHARED(defer_queue.q[head++ & DEFER_QUEUE_MASK], p);
	smp_wmb();	/* Publish new pointer before head */
			/* Write q[] before head. */
	STORE_SHARED(defer_queue.head, head);
	smp_mb();	/* Write queue head before read futex */
	/*
	 * Wake-up any waiting defer thread.
	 */
	wake_up_defer();
}

void *thr_defer(void *args)
{
	for (;;) {
		pthread_testcancel();
		/*
		 * "Be green". Don't wake up the CPU if there is no RCU work
		 * to perform whatsoever. Aims at saving laptop battery life by
		 * leaving the processor in sleep state when idle.
		 */
		wait_defer();
		/* Sleeping after wait_defer to let many callbacks enqueue */
		poll(NULL,0,100);	/* wait for 100ms */
		rcu_defer_barrier();
	}

	return NULL;
}

/*
 * library wrappers to be used by non-LGPL compatible source code.
 */

void rcu_defer_queue(void (*fct)(void *p), void *p)
{
	_rcu_defer_queue(fct, p);
}

static void rcu_add_deferer(pthread_t id)
{
	struct deferer_registry *oldarray;

	if (!registry) {
		alloc_deferers = INIT_NUM_THREADS;
		num_deferers = 0;
		registry =
			malloc(sizeof(struct deferer_registry) * alloc_deferers);
	}
	if (alloc_deferers < num_deferers + 1) {
		oldarray = registry;
		registry = malloc(sizeof(struct deferer_registry)
				* (alloc_deferers << 1));
		memcpy(registry, oldarray,
			sizeof(struct deferer_registry) * alloc_deferers);
		alloc_deferers <<= 1;
		free(oldarray);
	}
	registry[num_deferers].tid = id;
	/* reference to the TLS of _this_ deferer thread. */
	registry[num_deferers].defer_queue = &defer_queue;
	registry[num_deferers].last_head = 0;
	num_deferers++;
}

/*
 * Never shrink (implementation limitation).
 * This is O(nb threads). Eventually use a hash table.
 */
static void rcu_remove_deferer(pthread_t id)
{
	struct deferer_registry *index;

	assert(registry != NULL);
	for (index = registry; index < registry + num_deferers; index++) {
		if (pthread_equal(index->tid, id)) {
			memcpy(index, &registry[num_deferers - 1],
				sizeof(struct deferer_registry));
			registry[num_deferers - 1].tid = 0;
			registry[num_deferers - 1].defer_queue = NULL;
			registry[num_deferers - 1].last_head = 0;
			num_deferers--;
			return;
		}
	}
	/* Hrm not found, forgot to register ? */
	assert(0);
}

static void start_defer_thread(void)
{
	int ret;

	ret = pthread_create(&tid_defer, NULL, thr_defer,
		NULL);
	assert(!ret);
}

static void stop_defer_thread(void)
{
	int ret;
	void *tret;

	pthread_cancel(tid_defer);
	wake_up_defer();
	ret = pthread_join(tid_defer, &tret);
	assert(!ret);
}

void rcu_defer_register_thread(void)
{
	int deferers;

	internal_urcu_lock(&defer_thread_mutex);
	internal_urcu_lock(&urcu_defer_mutex);
	defer_queue.q = malloc(sizeof(void *) * DEFER_QUEUE_SIZE);
	rcu_add_deferer(pthread_self());
	deferers = num_deferers;
	internal_urcu_unlock(&urcu_defer_mutex);

	if (deferers == 1)
		start_defer_thread();
	internal_urcu_unlock(&defer_thread_mutex);
}

void rcu_defer_unregister_thread(void)
{
	int deferers;

	internal_urcu_lock(&defer_thread_mutex);
	internal_urcu_lock(&urcu_defer_mutex);
	rcu_remove_deferer(pthread_self());
	_rcu_defer_barrier_thread();
	free(defer_queue.q);
	defer_queue.q = NULL;
	deferers = num_deferers;
	internal_urcu_unlock(&urcu_defer_mutex);

	if (deferers == 0)
		stop_defer_thread();
	internal_urcu_unlock(&defer_thread_mutex);
}

void urcu_defer_exit(void)
{
	free(registry);
}
