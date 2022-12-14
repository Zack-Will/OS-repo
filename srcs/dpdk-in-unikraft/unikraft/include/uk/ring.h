/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Kip Macy <kmacy@freebsd.org>
 *          Alexander Jung <alexander.jung@neclab.eu>
 * 
 * Copyright (c) 2007-2009, Kip Macy <kmacy@freebsd.org>
 * Copyright (c) 2010-2017, Intel Corporation.
 * Copyright (c) 2020, NEC Laboratories Europe GmbH, NEC Corporation.
 *               All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef __UK_RING_H__
#define __UK_RING_H__

#include <errno.h>
#include <uk/mutex.h>
#include <uk/print.h>
#include <uk/config.h>
#include <uk/assert.h>
#include <uk/plat/lcpu.h>
#include <uk/arch/atomic.h>
#include <uk/essentials.h>
#include <uk/preempt.h>

#define critical_enter()  uk_preempt_disable()
#define critical_exit()   uk_preempt_enable()

#define __UK_RING_NAME0(x, ...) x
#define __UK_RING_NAME1(x, a1) UK_CONCAT(__UK_RING_NAME0(x), _ ## a1)
#define __UK_RING_NAME2(x, a1, a2) UK_CONCAT(__UK_RING_NAME1(x, a1), _ ## a2)
#define   UK_RING_NAME(x, ...) \
	UK_CONCAT(__UK_RING_NAME, UK_NARGS(__VA_ARGS__))(x, __VA_ARGS__)

#define UK_RING_STRUCT(br_name, br_type) \
	typedef struct UK_RING_NAME(br_name) { \
		volatile uint32_t br_prod_head; \
		volatile uint32_t br_prod_tail; \
		int               br_prod_size; \
		int               br_prod_mask; \
		uint64_t          br_drops; \
		volatile uint32_t br_cons_head __aligned(CACHE_LINE_SIZE); \
		volatile uint32_t br_cons_tail; \
		int               br_cons_size; \
		int               br_cons_mask; \
		struct uk_mutex  *br_lock;  \
		br_type           br_ring[0]; \
	} UK_RING_NAME(br_name, t)

#ifdef DEBUG_BUFRING
#define uk_ring_debug_set_elem(br, i, val) \
	br->br_ring[i] = val;
#else
#define uk_ring_debug_set_elem(br, i, val)
#endif /* DEBUG_BUFRING */

/**
 * This function updates the producer head for enqueue
 *
 * @param br
 *   A pointer to the ring structure
 * @param is_sp
 *   Indicates whether multi-producer path is needed or not
 * @param n
 *   The number of elements we will want to enqueue, i.e. how far should the
 *   head be moved
 * @param old_head
 *   Returns head value as it was before the move, i.e. where enqueue starts
 * @param new_head
 *   Returns the current/new head value i.e. where enqueue finishes
 * @param free_entries
 *   Returns the amount of free space in the ring BEFORE head was moved
 * @return
 *   Actual number of objects enqueued.
 */
#define UK_RING_MOVE_PROD_HEAD(br_name, br, is_sp, n, old_head, new_head, \
					free_entries) UK_RING_NAME(br_name, move_prod_head_mc)(br, is_sp, n, \
					old_head, new_head, free_entries)

#define UK_RING_MOVE_PROD_HEAD_FN(br_name, br_type) \
	static __inline unsigned int \
	UK_RING_NAME(br_name, move_prod_head)(UK_RING_NAME(br_name, t) *br, \
					unsigned int is_sp, unsigned int n, uint32_t *old_head, \
					uint32_t *new_head, uint32_t *free_entries) \
	{ \
		const uint32_t capacity = br->br_prod_size; \
		unsigned int max = n; \
		int success; \
		do { \
			/* Reset n to the initial burst count */\
			n = max; \
			*old_head = br->br_prod_head; \
			/* add rmb barrier to avoid load/load reorder in weak \
			 * memory model. It is noop on x86 \
			 */\
			rmb(); \
			/*\
			 * The subtraction is done between two unsigned 32bits value \
			 * (the result is always modulo 32 bits even if we have \
			 * *old_head > cons_tail). So 'free_entries' is always between 0 \
			 * and capacity (which is < size). \
			 */\
			*free_entries = (capacity + br->br_cons_tail - *old_head); \
			/* check that we have enough room in ring */\
			if (unlikely(n > *free_entries)) \
				n = *free_entries; \
			if (n == 0) \
				return 0; \
			*new_head = *old_head + n; \
			if (is_sp) \
				br->br_prod_head = *new_head, success = 1; \
			else \
				success = ukarch_compare_exchange_sync(&br->br_prod_head, *old_head, \
					*new_head); \
		} while (unlikely(success == 0)); \
		return n; \
	}

/**
 * This function updates the consumer head for dequeue
 *
 * @param br
 *   A pointer to the ring structure
 * @param is_sc
 *   Indicates whether multi-consumer path is needed or not
 * @param n
 *   The number of elements we will want to enqueue, i.e. how far should the
 *   head be moved
 * @param old_head
 *   Returns head value as it was before the move, i.e. where dequeue starts
 * @param new_head
 *   Returns the current/new head value i.e. where dequeue finishes
 * @param entries
 *   Returns the number of entries in the ring BEFORE head was moved
 * @return
 *   Actual number of objects dequeued.
 */
#define UK_RING_MOVE_CONS_HEAD(br_name, br, is_sc, n, old_head, new_head, \
					free_entries) UK_RING_NAME(br_name, move_cons_head_mc)(br, is_sc, n, \
					old_head, new_head, free_entries)

#define UK_RING_MOVE_CONS_HEAD_FN(br_name, br_type) \
	static __inline unsigned int \
	UK_RING_NAME(br_name, move_cons_head)(UK_RING_NAME(br_name, t) *br, \
					unsigned int is_sc, unsigned int n, uint32_t *old_head, \
					uint32_t *new_head, uint32_t *entries) \
	{ \
		unsigned int max = n; \
		int success; \
		/* move br_cons_head atomically */\
		do { \
			/* Restore n as it may change every loop */\
			n = max; \
			*old_head = br->br_cons_head; \
			/* add rmb barrier to avoid load/load reorder in weak \
			 * memory model. It is noop on x86 \
			 */\
			rmb(); \
			/* The subtraction is done between two unsigned 32bits value \
			 * (the result is always modulo 32 bits even if we have \
			 * cons_head > prod_tail). So 'entries' is always between 0 \
			 * and size(ring)-1. \
			 */\
			*entries = (br->br_prod_tail - *old_head); \
			/* Set the actual entries for dequeue */\
			if (n > *entries) \
				n = *entries; \
			if (unlikely(n == 0)) \
				return 0; \
			*new_head = *old_head + n; \
			if (is_sc) \
				br->br_cons_head = *new_head, success = 1; \
			else \
				success = ukarch_compare_exchange_sync(&br->br_cons_head, *old_head, \
						*new_head); \
		} while (unlikely(success == 0)); \
		return n; \
	}

/*
 * multi-producer safe lock-free ring buffer enqueue
 *
 */
#define UK_RING_ENQUEUE(br_name, br, buf) \
					UK_RING_NAME(br_name, enqueue)(br, buf)

#define uk_ring_while_next(ptr) \
	while (!ukarch_compare_exchange_sync((uint32_t *) &br->br_ ## ptr ## _head, \
		ptr ## _head, ptr ## _next));

#ifdef DEBUG_BUFRING
/*
 * Note: It is possible to encounter an mbuf that was removed
 * via drbr_peek(), and then re-added via drbr_putback() and
 * trigger a spurious panic.
 */
#define uk_ring_check_already_enqueued(br, buf) \
	int i; \
	for (i = br->br_cons_head; i != br->br_prod_head; \
			 i = ((i + 1) & br->br_cons_mask)) \
		if (br->br_ring[i] == buf) \
			UK_CRASH("buf=%p already enqueue at %d prod=%d cons=%d", 
					buf, i, br->br_prod_tail, br->br_cons_tail)
#else
#define uk_ring_check_already_enqueued(br, buf)
#endif /* DEBUG_BUFRING */

#define UK_RING_ENQUEUE_FN(br_name, br_type) \
	static __inline int \
	UK_RING_NAME(br_name, enqueue)(UK_RING_NAME(br_name, t) *br, br_type buf) \
	{ \
		uint32_t prod_head, prod_next, cons_tail; \
		uk_ring_check_already_enqueued(br, buf); \
		critical_enter(); \
		do { \
			prod_head = br->br_prod_head; \
			prod_next = (prod_head + 1) & br->br_prod_mask; \
			cons_tail = br->br_cons_tail; \
			if (prod_next == cons_tail) { \
				rmb(); \
				if (prod_head == br->br_prod_head && cons_tail == br->br_cons_tail) { \
					br->br_drops++; \
					critical_exit(); \
					return -ENOBUFS; \
				} \
				continue; \
			} \
		} uk_ring_while_next(prod); \
		br->br_ring[prod_head] = buf; \
		/*\
		 * If there are other enqueues in progress \
		 * that preceded us, we need to wait for them \
		 * to complete \
		 */\
		while (br->br_prod_tail != prod_head) \
			ukarch_spinwait(); \
		ukarch_store_n(&br->br_prod_tail, prod_next); \
		critical_exit(); \
		return 0; \
	}

/**
 * Enqueue several objects on the ring
 *
 * @param br
 *   A pointer to the ring structure.
 * @param buf
 *   A pointer to the buffer to enqueue from.
 * @param n
 *   The number of objects to add in the ring from the obj_table.
 * @param free_space
 *   returns the amount of space after the enqueue operation has finished
 * @return
 *   Actual number of objects enqueued.
 */
#define UK_RING_ENQUEUE_BULK_MC(br_name, br, buf, n, free_space) \
					UK_RING_NAME(br_name, enqueue_bulk_mc)(br, n, buf, free_space)

#define UK_RING_ENQUEUE_BULK_MC_FN(br_name, br_type) \
	static __inline unsigned int \
	UK_RING_NAME(br_name, enqueue_bulk_mc)(UK_RING_NAME(br_name, t) *br, \
					br_type buf[], unsigned int n, unsigned int *free_space) \
	{ \
		unsigned int i; \
		uint32_t idx; \
		uint32_t prod_head, prod_next; \
		uint32_t free_entries; \
		n = UK_RING_NAME(br_name, move_prod_head)(br, 0, n, &prod_head, \
					&prod_next, &free_entries); \
		if (n == 0) \
			goto end; \
		idx = prod_head & br->br_prod_mask; \
		if (likely(idx + n < br->br_prod_size)) { \
			for (i = 0; i < (n & ((~(unsigned)0x3))); i+=4, idx+=4) { \
				br->br_ring[idx] = buf[i]; \
				br->br_ring[idx+1] = buf[i+1]; \
				br->br_ring[idx+2] = buf[i+2]; \
				br->br_ring[idx+3] = buf[i+3]; \
			} \
			switch (n & 0x3) { \
			case 3: \
				br->br_ring[idx++] = buf[i++]; /* fallthrough */ \
			case 2: \
				br->br_ring[idx++] = buf[i++]; /* fallthrough */ \
			case 1: \
				br->br_ring[idx++] = buf[i++]; \
			} \
		} else { \
			for (i = 0; idx < br->br_prod_size; i++, idx++)\
				br->br_ring[idx] = buf[i]; \
			for (idx = 0; i < n; i++, idx++) \
				br->br_ring[idx] = buf[i]; \
		} \
		wmb(); \
		while (br->br_prod_tail != prod_head) \
			ukarch_spinwait(); \
		ukarch_store_n(&br->br_prod_tail, prod_next); \
	end: \
		if (free_space != NULL) \
			*free_space = free_entries - n; \
		return n; \
	}

/**
 * Dequeue several objects from the ring
 *
 * @param br
 *   A pointer to the ring structure.
 * @param buf
 *   A pointer to the buffer to dequeue objects into.
 * @param n
 *   The number of objects to pull from the ring.
 * @param available
 *   returns the number of remaining ring entries after the dequeue has finished
 * @return
 *   Actual number of objects dequeued.
 */
#define UK_RING_DEQUEUE_BULK_MC(br_name, br, buf, n, available) \
					UK_RING_NAME(br_name, dequeue_bulk_mc)(br, n, buf, available)

#define UK_RING_DEQUEUE_BULK_MC_FN(br_name, br_type) \
	static __inline unsigned int \
	UK_RING_NAME(br_name, dequeue_bulk_mc)(UK_RING_NAME(br_name, t) *br, \
					br_type buf[], unsigned int n, unsigned int *available) \
	{ \
		unsigned int i; \
		uint32_t idx; \
		uint32_t cons_head, cons_next; \
		uint32_t entries; \
		n = UK_RING_NAME(br_name, move_cons_head)(br, 0, n,  &cons_head, \
					&cons_next, &entries); \
		if (n == 0) \
			goto end; \
		idx = cons_head & br->br_cons_mask; \
		if (likely(idx + n < br->br_cons_size)) { \
			for (i = 0; i < (n & (~(unsigned)0x3)); i+=4, idx+=4) {\
				buf[i] = br->br_ring[idx]; \
				buf[i+1] = br->br_ring[idx+1]; \
				buf[i+2] = br->br_ring[idx+2]; \
				buf[i+3] = br->br_ring[idx+3]; \
			} \
			switch (n & 0x3) { \
			case 3: \
				buf[i++] = br->br_ring[idx++]; /* fallthrough */ \
			case 2: \
				buf[i++] = br->br_ring[idx++]; /* fallthrough */ \
			case 1: \
				buf[i++] = br->br_ring[idx++]; \
			} \
		} else { \
			for (i = 0; idx < br->br_cons_size; i++, idx++) \
				buf[i] = br->br_ring[idx]; \
			for (idx = 0; i < n; i++, idx++) \
				buf[i] = br->br_ring[idx]; \
		} \
		rmb(); \
		/*\
		 * If there are other enqueues/dequeues in progress that preceded us, \
		 * we need to wait for them to complete \
		 */\
		while (unlikely(br->br_cons_tail != cons_head)) \
			ukarch_spinwait(); \
		br->br_cons_tail = cons_next; \
	end: \
		if (available != NULL) \
			*available = entries - n; \
		return n; \
	}

/*
 * multi-consumer safe dequeue 
 *
 */
#define UK_RING_DEQUEUE_MC(br_name, br) UK_RING_NAME(br_name, dequeue_mc)(br)

#define UK_RING_DEQUEUE_MC_FN(br_name, br_type) \
	static __inline br_type \
	UK_RING_NAME(br_name, dequeue_mc)(UK_RING_NAME(br_name, t) *br) \
	{ \
		uint32_t cons_head, cons_next; \
		br_type buf; \
		critical_enter(); \
		do { \
			cons_head = br->br_cons_head; \
			cons_next = (cons_head + 1) & br->br_cons_mask; \
			if (cons_head == br->br_prod_tail) { \
				critical_exit(); \
				return NULL; \
			} \
		} uk_ring_while_next(cons); \
		buf = br->br_ring[cons_head]; \
		uk_ring_debug_set_elem(br, cons_head, NULL); \
		/*\
		 * If there are other dequeues in progress that preceded us, we need to \
		 * wait for them to complete. \
		 */\
		while (br->br_cons_tail != cons_head) \
			ukarch_spinwait(); \
		ukarch_store_n(&br->br_cons_tail, cons_next); \
		critical_exit(); \
		return buf; \
	}

/*
 * single-consumer dequeue 
 * use where dequeue is protected by a lock
 * e.g. a network driver's tx queue lock
 */
#define UK_RING_DEQUEUE_SC(br_name, br) UK_RING_NAME(br_name, dequeue_sc)(br)

	/*
	 * This is a workaround to allow using uk_ring on ARM and ARM64.
	 * ARM64TODO: Fix uk_ring in a generic way.
	 * REMARKS: It is suspected that br_cons_head does not require
	 *   load_acq operation, but this change was extensively tested
	 *   and confirmed it's working. To be reviewed once again in
	 *   FreeBSD-12.
	 *
	 * Preventing following situation:
	 * Core(0) - uk_ring_enqueue()                                       Core(1) - uk_ring_dequeue_sc()
	 * -----------------------------------------                                       ----------------------------------------------
	 *
	 *                                                                                cons_head = br->br_cons_head;
	 * atomic_cmpset_acq_32(&br->br_prod_head, ...));
	 *                                                                                buf = br->br_ring[cons_head];     <see <1>>
	 * br->br_ring[prod_head] = buf;
	 * atomic_store_rel_32(&br->br_prod_tail, ...);
	 *                                                                                prod_tail = br->br_prod_tail;
	 *                                                                                if (cons_head == prod_tail) 
	 *                                                                                        return (NULL);
	 *                                                                                <condition is false and code uses invalid(old) buf>`
	 *
	 * <1> Load (on core 1) from br->br_ring[cons_head] can be reordered (speculative readed) by CPU.
	 */
#if defined(CONFIG_ARCH_ARM_32) || defined(CONFIG_ARCH_ARM_64)
#define uk_ring_set_cons_head(br, cons_head) \
	cons_head = ukarch_load_n(&br->br_cons_head)
#else
#define uk_ring_set_cons_head(br, cons_head) \
	cons_head = br->br_cons_head
#endif

#ifdef PREFETCH_DEFINED
#define uk_ring_cons_next_next uint32_t cons_next_next
#define uk_ring_set_cons_next_next(br, cons_head, cons_next_next) \
	cons_next_next = (cons_head + 2) & br->br_cons_mask;
#define uk_ring_prefetch_next(br, cons_next, prod_tail, cons_next_next) \
	if (cons_next != prod_tail) { \
		prefetch(br->br_ring[cons_next]); \
		if (cons_next_next != prod_tail) \
			prefetch(br->br_ring[cons_next_next]); \
	}
#else
#define uk_ring_cons_next_next
#define uk_ring_set_cons_next_next(br, cons_head, cons_next_next)
#define uk_ring_prefetch_next(br, cons_next, prod_tail, cons_next_next)
#endif /* PREFETCH_DEFINED */

#ifdef DEBUG_BUFRING
#define uk_ring_debug_check_lock(br) \
	if (!uk_mutex_is_locked(br->br_lock)) \
		UK_CRASH("lock not held on single consumer dequeue: %d", \
			br->br_lock->lock_count); \
#define uk_ring_debug_check_cons_head_tail(br, cons_head) \
	uk_ring_debug_set_elem(br, cons_head, NULL); \
	uk_ring_debug_check_lock(); \
	if (br->br_cons_tail != cons_head) \
		UK_CRASH("inconsistent list cons_tail=%d cons_head=%d", \
				br->br_cons_tail, cons_head)
#else
#define uk_ring_debug_check_lock(br)
#define uk_ring_debug_check_cons_head_tail(br, cons_head)
#endif /* DEBUG_BUFRING */

#define UK_RING_DEQUEUE_SC_FN(br_name, br_type) \
	static __inline br_type \
	UK_RING_NAME(br_name, dequeue_sc)(UK_RING_NAME(br_name, t) *br) \
	{ \
		uint32_t cons_head, cons_next; \
		uk_ring_cons_next_next; \
		uint32_t prod_tail; \
		br_type buf; \
		uk_ring_set_cons_head(br, cons_head); \
		cons_next = (cons_head + 1) & br->br_cons_mask; \
		uk_ring_set_cons_next_next(br, cons_head, cons_next_next); \
		if (cons_head == prod_tail) \
			return NULL; \
		uk_ring_prefetch_next(br, cons_next, prod_tail, cons_next_next); \
		br->br_cons_head = cons_next; \
		buf = br->br_ring[cons_head]; \
		uk_ring_debug_check_cons_head_tail(br, cons_head); \
		br->br_cons_tail = cons_next; \
		return buf; \
	}

/*
 * single-consumer advance after a peek
 * use where it is protected by a lock
 * e.g. a network driver's tx queue lock
 */
#define UK_RING_ADVANCE_SC(br_name, br) UK_RING_NAME(br_name, advance_sc)(br)

#define UK_RING_ADVANCE_SC_FN(br_name, br_type) \
	static __inline void \
	UK_RING_NAME(br_name, advance_sc)(UK_RING_NAME(br_name, t) *br) \
	{ \
		uint32_t cons_head, cons_next; \
		uint32_t prod_tail; \
		cons_head = br->br_cons_head; \
		prod_tail = br->br_prod_tail; \
		cons_next = (cons_head + 1) & br->br_cons_mask; \
		if (cons_head == prod_tail) \
			return; \
		br->br_cons_head = cons_next; \
		uk_ring_debug_set_elem(br, cons_head, NULL); \
		br->br_cons_tail = cons_next; \
	}

/*
 * Used to return a buffer (most likely already there)
 * to the top of the ring. The caller should *not*
 * have used any dequeue to pull it out of the ring
 * but instead should have used the peek() function.
 * This is normally used where the transmit queue
 * of a driver is full, and an mbuf must be returned.
 * Most likely whats in the ring-buffer is what
 * is being put back (since it was not removed), but
 * sometimes the lower transmit function may have
 * done a pullup or other function that will have
 * changed it. As an optimization we always put it
 * back (since jhb says the store is probably cheaper),
 * if we have to do a multi-queue version we will need
 * the compare and an atomic.
 */
#define UK_RING_PUTBACK_SC(br_name, br, new) \
	UK_RING_NAME(br_name, putback_sc)(br, new)

#define UK_RING_PUTBACK_SC_FN(br_name, br_type) \
	static __inline void \
	UK_RING_NAME(br_name, putback_sc)(UK_RING_NAME(br_name, t) *br, br_type new) \
	{ \
		/* Buffer ring has none in putback */\
		UK_ASSERT(br->br_cons_head != br->br_prod_tail); \
		br->br_ring[br->br_cons_head] = new; \
	}

/*
 * return a pointer to the first entry in the ring
 * without modifying it, or NULL if the ring is empty
 * race-prone if not protected by a lock
 */
#define UK_RING_PEEK(br_name, br) UK_RING_NAME(br_name, peek)(br)

#define UK_RING_PEEK_FN(br_name, br_type) \
	static __inline br_type \
	UK_RING_NAME(br_name, peek)(UK_RING_NAME(br_name, t) *br) \
	{ \
		uk_ring_debug_check_lock(br); \
		/*\
		 * I believe it is safe to not have a memory barrier \
		 * here because we control cons and tail is worst case \
		 * a lagging indicator so we worst case we might \
		 * return NULL immediately after a buffer has been enqueued \
		 */\
		if (br->br_cons_head == br->br_prod_tail) \
			return NULL; \
		return br->br_ring[br->br_cons_head]; \
	}

#if defined(CONFIG_ARCH_ARM_32) || defined(CONFIG_ARCH_ARM_64)
	/*
	 * The barrier is required there on ARM and ARM64 to ensure, that
	 * br->br_ring[br->br_cons_head] will not be fetched before the above
	 * condition is checked.
	 * Without the barrier, it is possible, that buffer will be fetched
	 * before the enqueue will put mbuf into br, then, in the meantime, the
	 * enqueue will update the array and the br_prod_tail, and the
	 * conditional check will be true, so we will return previously fetched
	 * (and invalid) buffer.
	 */
#define UK_RING_PEEK_CLEAR_SC(br_name, br) \
	#error "unsupported: atomic_thread_fence_acq()"
#else
#define UK_RING_PEEK_CLEAR_SC(br_name, br) \
					UK_RING_NAME(br_name, peek_clear_sc)(br)
#endif /* defined(CONFIG_ARCH_ARM_32) || defined(CONFIG_ARCH_ARM_64) */

#define UK_RING_PEEK_CLEAR_SC_FN(br_name, br_type) \
	static __inline br_type \
	UK_RING_NAME(br_name, peek_clear_sc)(UK_RING_NAME(br_name, t) *br) \
	{ \
		br_type ret; \
		uk_ring_debug_check_lock(br); \
		if (br->br_cons_head == br->br_prod_tail) \
			return NULL; \
		ret = br->br_ring[br->br_cons_head]; \
		/*\
		 * Single consumer, i.e. cons_head will not move while we are \
		 * running, so atomic_swap_ptr() is not necessary here. \
		 */\
		uk_ring_debug_set_elem(br, br->br_cons_head, NULL); \
		return ret; \
	}

#define UK_RING_FULL(br_name, br) UK_RING_NAME(br_name, full)(br)

#define UK_RING_FULL_FN(br_name, br_type) \
	static __inline int \
	UK_RING_NAME(br_name, full)(UK_RING_NAME(br_name, t) *br) \
	{ \
		return ((br->br_prod_head + 1) & br->br_prod_mask) == br->br_cons_tail; \
	}

#define UK_RING_EMPTY(br_name, br) UK_RING_NAME(br_name, empty)(br)

#define UK_RING_EMPTY_FN(br_name, type) \
	static __inline int \
	UK_RING_NAME(br_name, empty)(UK_RING_NAME(br_name, t) *br) \
	{ \
		return br->br_cons_head == br->br_prod_tail; \
	}

#define UK_RING_COUNT(br_name, br) UK_RING_NAME(br_name, count)(br)

#define UK_RING_COUNT_FN(br_name, br_type) \
	static __inline int \
	UK_RING_NAME(br_name, count)(UK_RING_NAME(br_name, t) *br) \
	{ \
		return (br->br_prod_size + br->br_prod_tail - br->br_cons_tail) \
				& br->br_prod_mask; \
	}

#ifdef DEBUG_BUFRING
#define UK_RING_ALLOC(br_name, count, a, lock) \
					UK_RING_NAME(br_name, alloc)(br_name, count, a, lock)

#define UK_RING_ALLOC_FN(br_name, br_type) \
	static __inline UK_RING_NAME(br_name, t) * \
	UK_RING_NAME(br_name, alloc)(unsigned int count, struct uk_alloc *a, \
					struct uk_mutex *lock) \
	{ \
		UK_RING_NAME(br_name, t) *br; \
		/* buf ring must be size power of 2 */\
		UK_ASSERT(POWER_OF_2(count)); \
		br = uk_malloc(a, sizeof(UK_RING_NAME(br_name, t)) + \
					count * sizeof(br_type)); \
		if (br == NULL) \
			return NULL; \
		br->br_lock = lock;
		br->br_prod_size = br->br_cons_size = count; \
		br->br_prod_mask = br->br_cons_mask = count - 1; \
		br->br_prod_head = br->br_cons_head = 0; \
		br->br_prod_tail = br->br_cons_tail = 0; \
		return br; \
	}
#else
#define UK_RING_ALLOC(br_name, count, a) \
					UK_RING_NAME(br_name, alloc)(br_name, count, a)

#define UK_RING_ALLOC_FN(br_name, br_type) \
	static __inline UK_RING_NAME(br_name, t) * \
	UK_RING_NAME(br_name, alloc)(unsigned int count, struct uk_alloc *a) \
	{ \
		UK_RING_NAME(br_name, t) *br; \
		/* buf ring must be size power of 2 */\
		UK_ASSERT(POWER_OF_2(count)); \
		br = uk_malloc(a, sizeof(UK_RING_NAME(br_name, t)) + \
					count * sizeof(br_type)); \
		if (br == NULL) \
			return NULL; \
		br->br_prod_size = br->br_cons_size = count; \
		br->br_prod_mask = br->br_cons_mask = count - 1; \
		br->br_prod_head = br->br_cons_head = 0; \
		br->br_prod_tail = br->br_cons_tail = 0; \
		return br; \
	}
#endif /* DEBUG_BUFRING */

#define UK_RING_FREE(br_name, br, a) UK_RING_NAME(br_name, free)(br, a)

#define UK_RING_FREE_FN(br_name, br_type) \
	void \
	UK_RING_NAME(br_name, free)(UK_RING_NAME(br_name, t) *br, \
					struct uk_alloc *a) \
	{ \
		uk_free(a, br); \
	}

#define UK_RING_DEFINE(br_name, br_type) \
	UK_RING_STRUCT(br_name, br_type); \
	UK_RING_ALLOC_FN(br_name, br_type); \
	UK_RING_FREE_FN(br_name, br_type); \
	UK_RING_MOVE_PROD_HEAD_FN(br_name, br_type); \
	UK_RING_ENQUEUE_FN(br_name, br_type); \
	UK_RING_ENQUEUE_BULK_MC_FN(br_name, br_type); \
	UK_RING_MOVE_CONS_HEAD_FN(br_name, br_type); \
	UK_RING_DEQUEUE_SC_FN(br_name, br_type); \
	UK_RING_DEQUEUE_MC_FN(br_name, br_type); \
	UK_RING_DEQUEUE_BULK_MC_FN(br_name, br_type); \
	UK_RING_PUTBACK_SC_FN(br_name, br_type); \
	UK_RING_PEEK_FN(br_name, br_type); \
	UK_RING_PEEK_CLEAR_SC_FN(br_name, br_type); \
	UK_RING_FULL_FN(br_name, br_type); \
	UK_RING_EMPTY_FN(br_name, br_type); \
	UK_RING_COUNT_FN(br_name, br_type)

#endif
