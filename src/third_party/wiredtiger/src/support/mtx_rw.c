/*-
 * Public Domain 2014-2016 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Based on "Spinlocks and Read-Write Locks" by Dr. Steven Fuerst:
 *	http://locklessinc.com/articles/locks/
 *
 * Dr. Fuerst further credits:
 *	There exists a form of the ticket lock that is designed for read-write
 * locks. An example written in assembly was posted to the Linux kernel mailing
 * list in 2002 by David Howells from RedHat. This was a highly optimized
 * version of a read-write ticket lock developed at IBM in the early 90's by
 * Joseph Seigh. Note that a similar (but not identical) algorithm was published
 * by John Mellor-Crummey and Michael Scott in their landmark paper "Scalable
 * Reader-Writer Synchronization for Shared-Memory Multiprocessors".
 *
 * The following is an explanation of this code. First, the underlying lock
 * structure.
 *
 *	struct {
 *		uint16_t writers;	Now serving for writers
 *		uint16_t readers;	Now serving for readers
 *		uint16_t next;		Next available ticket number
 *		uint16_t __notused;	Padding
 *	}
 *
 * First, imagine a store's 'take a number' ticket algorithm. A customer takes
 * a unique ticket number and customers are served in ticket order. In the data
 * structure, 'writers' is the next writer to be served, 'readers' is the next
 * reader to be served, and 'next' is the next available ticket number.
 *
 * Next, consider exclusive (write) locks. The 'now serving' number for writers
 * is 'writers'. To lock, 'take a number' and wait until that number is being
 * served; more specifically, atomically copy and increment the current value of
 * 'next', and then wait until 'writers' equals that copied number.
 *
 * Shared (read) locks are similar. Like writers, readers atomically get the
 * next number available. However, instead of waiting for 'writers' to equal
 * their number, they wait for 'readers' to equal their number.
 *
 * This has the effect of queuing lock requests in the order they arrive
 * (incidentally avoiding starvation).
 *
 * Each lock/unlock pair requires incrementing both 'readers' and 'writers'.
 * In the case of a reader, the 'readers' increment happens when the reader
 * acquires the lock (to allow read-lock sharing), and the 'writers' increment
 * happens when the reader releases the lock. In the case of a writer, both
 * 'readers' and 'writers' are incremented when the writer releases the lock.
 *
 * For example, consider the following read (R) and write (W) lock requests:
 *
 *						writers	readers	next
 *						0	0	0
 *	R: ticket 0, readers match	OK	0	1	1
 *	R: ticket 1, readers match	OK	0	2	2
 *	R: ticket 2, readers match	OK	0	3	3
 *	W: ticket 3, writers no match	block	0	3	4
 *	R: ticket 2, unlock			1	3	4
 *	R: ticket 0, unlock			2	3	4
 *	R: ticket 1, unlock			3	3	4
 *	W: ticket 3, writers match	OK	3	3	4
 *
 * Note the writer blocks until 'writers' equals its ticket number and it does
 * not matter if readers unlock in order or not.
 *
 * Readers or writers entering the system after the write lock is queued block,
 * and the next ticket holder (reader or writer) will unblock when the writer
 * unlocks. An example, continuing from the last line of the above example:
 *
 *						writers	readers	next
 *	W: ticket 3, writers match	OK	3	3	4
 *	R: ticket 4, readers no match	block	3	3	5
 *	R: ticket 5, readers no match	block	3	3	6
 *	W: ticket 6, writers no match	block	3	3	7
 *	W: ticket 3, unlock			4	4	7
 *	R: ticket 4, readers match	OK	4	5	7
 *	R: ticket 5, readers match	OK	4	6	7
 *
 * The 'next' field is a 2-byte value so the available ticket number wraps at
 * 64K requests. If a thread's lock request is not granted until the 'next'
 * field cycles and the same ticket is taken by another thread, we could grant
 * a lock to two separate threads at the same time, and bad things happen: two
 * writer threads or a reader thread and a writer thread would run in parallel,
 * and lock waiters could be skipped if the unlocks race. This is unlikely, it
 * only happens if a lock request is blocked by 64K other requests. The fix is
 * to grow the lock structure fields, but the largest atomic instruction we have
 * is 8 bytes, the structure has no room to grow.
 */

#include "wt_internal.h"

/*
 * __wt_rwlock_init --
 *	Initialize a read/write lock.
 */
void
__wt_rwlock_init(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
	WT_UNUSED(session);

	l->u = 0;
}

/*
 * __wt_rwlock_destroy --
 *	Destroy a read/write lock.
 */
void
__wt_rwlock_destroy(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
	WT_UNUSED(session);

	l->u = 0;
}

/*
 * __wt_try_readlock --
 *	Try to get a shared lock, fail immediately if unavailable.
 */
int
__wt_try_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
	WT_RWLOCK new, old;

	WT_STAT_CONN_INCR(session, rwlock_read);

	new = old = *l;

	/*
	 * This read lock can only be granted if the lock was last granted to
	 * a reader and there are no readers or writers blocked on the lock,
	 * that is, if this thread's ticket would be the next ticket granted.
	 * Do the cheap test to see if this can possibly succeed (and confirm
	 * the lock is in the correct state to grant this read lock).
	 */
	if (old.s.readers != old.s.next)
		return (EBUSY);

	/*
	 * The replacement lock value is a result of allocating a new ticket and
	 * incrementing the reader value to match it.
	 */
	new.s.readers = new.s.next = old.s.next + 1;
	return (__wt_atomic_cas64(&l->u, old.u, new.u) ? 0 : EBUSY);
}

/*
 * __wt_readlock_spin --
 *	Spin to get a read lock: only yield the CPU if the lock is held
 *	exclusive.
 */
void
__wt_readlock_spin(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
	/*
	 * Try to get the lock in a single operation if it is available to
	 * readers.  This avoids the situation where multiple readers arrive
	 * concurrently and have to line up in order to enter the lock.  For
	 * read-heavy workloads it can make a significant difference.
	 */
	while (__wt_try_readlock(session, l) != 0) {
		if (l->s.writers_active > 0)
			__wt_yield();
		else
			WT_PAUSE();
	}
}

/*
 * __wt_readlock --
 *	Get a shared lock.
 */
void
__wt_readlock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
	uint16_t ticket;
	int pause_cnt;

	WT_STAT_CONN_INCR(session, rwlock_read);

	WT_DIAGNOSTIC_YIELD;

	/*
	 * Possibly wrap: if we have more than 64K lockers waiting, the ticket
	 * value will wrap and two lockers will simultaneously be granted the
	 * lock.
	 */
	ticket = __wt_atomic_fetch_add16(&l->s.next, 1);
	for (pause_cnt = 0; ticket != l->s.readers;) {
		/*
		 * We failed to get the lock; pause before retrying and if we've
		 * paused enough, yield so we don't burn CPU to no purpose. This
		 * situation happens if there are more threads than cores in the
		 * system and we're thrashing on shared resources.
		 */
		if (++pause_cnt < WT_THOUSAND)
			WT_PAUSE();
		else
			__wt_yield();
	}

	/*
	 * We're the only writer of the readers field, so the update does not
	 * need to be atomic.
	 */
	++l->s.readers;

	/*
	 * Applications depend on a barrier here so that operations holding the
	 * lock see consistent data.
	 */
	WT_READ_BARRIER();
}

/*
 * __wt_readunlock --
 *	Release a shared lock.
 */
void
__wt_readunlock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
	WT_UNUSED(session);

	/*
	 * Increment the writers value (other readers are doing the same, make
	 * sure we don't race).
	 */
	(void)__wt_atomic_add16(&l->s.writers, 1);
}

/*
 * __wt_try_writelock --
 *	Try to get an exclusive lock, fail immediately if unavailable.
 */
int
__wt_try_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
	WT_RWLOCK new, old;

	WT_STAT_CONN_INCR(session, rwlock_write);

	old = new = *l;

	/*
	 * This write lock can only be granted if the lock was last granted to
	 * a writer and there are no readers or writers blocked on the lock,
	 * that is, if this thread's ticket would be the next ticket granted.
	 * Do the cheap test to see if this can possibly succeed (and confirm
	 * the lock is in the correct state to grant this write lock).
	 */
	if (old.s.writers != old.s.next)
		return (EBUSY);

	/* The replacement lock value is a result of allocating a new ticket. */
	++new.s.next;
	++new.s.writers_active;
	return (__wt_atomic_cas64(&l->u, old.u, new.u) ? 0 : EBUSY);
}

/*
 * __wt_writelock --
 *	Wait to get an exclusive lock.
 */
void
__wt_writelock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
	uint16_t ticket;
	int pause_cnt;

	WT_STAT_CONN_INCR(session, rwlock_write);

	/*
	 * Possibly wrap: if we have more than 64K lockers waiting, the ticket
	 * value will wrap and two lockers will simultaneously be granted the
	 * lock.
	 */
	ticket = __wt_atomic_fetch_add16(&l->s.next, 1);
	(void)__wt_atomic_add16(&l->s.writers_active, 1);
	for (pause_cnt = 0; ticket != l->s.writers;) {
		/*
		 * We failed to get the lock; pause before retrying and if we've
		 * paused enough, sleep so we don't burn CPU to no purpose. This
		 * situation happens if there are more threads than cores in the
		 * system and we're thrashing on shared resources.
		 */
		if (++pause_cnt < WT_THOUSAND)
			WT_PAUSE();
		else
			__wt_sleep(0, 10);
	}

	/*
	 * Applications depend on a barrier here so that operations holding the
	 * lock see consistent data.
	 */
	WT_READ_BARRIER();
}

/*
 * __wt_writeunlock --
 *	Release an exclusive lock.
 */
void
__wt_writeunlock(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
	WT_RWLOCK new;

	WT_UNUSED(session);

	(void)__wt_atomic_sub16(&l->s.writers_active, 1);

	/*
	 * Ensure that all updates made while the lock was held are visible to
	 * the next thread to acquire the lock.
	 */
	WT_WRITE_BARRIER();

	new = *l;

	/*
	 * We're the only writer of the writers/readers fields, so the update
	 * does not need to be atomic; we have to update both values at the
	 * same time though, otherwise we'd potentially race with the thread
	 * next granted the lock.
	 */
	++new.s.writers;
	++new.s.readers;
	l->i.wr = new.i.wr;

	WT_DIAGNOSTIC_YIELD;
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_rwlock_islocked --
 *	Return if a read/write lock is currently locked for reading or writing.
 */
bool
__wt_rwlock_islocked(WT_SESSION_IMPL *session, WT_RWLOCK *l)
{
	WT_UNUSED(session);

	return (l->s.writers != l->s.next || l->s.readers != l->s.next);
}
#endif
