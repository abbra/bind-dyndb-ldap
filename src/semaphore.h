/*
 * Authors: Martin Nagy <mnagy@redhat.com>
 *
 * Copyright (C) 2008  Red Hat
 * see file 'COPYING' for use and warranty information
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 or later
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

#ifndef _LD_SEMAPHORE_H_
#define _LD_SEMAPHORE_H_

#include <isc/condition.h>
#include <isc/mutex.h>

/* Multiplier for to user-defined connection parameter 'timeout'. */
#define SEM_WAIT_TIMEOUT_MUL 6 /* times */
extern isc_interval_t semaphore_wait_timeout;

/*
 * Semaphore can be "acquired" multiple times. However, it has a maximum
 * number of times someone can acquire him. If a semaphore is already acquired
 * more times than allowed, it will block until other thread release its,
 */
struct semaphore {
	int value;		/* Maximum number of times you can LOCK()) */
	isc_mutex_t mutex;	/* Mutex protecting this whole struct.     */
	isc_condition_t cond;	/* Condition used for waiting on release.  */
};

typedef struct semaphore	semaphore_t;

/* Public functions. */
isc_result_t	semaphore_init(semaphore_t *sem, int value);
void		semaphore_destroy(semaphore_t *sem);
isc_result_t	semaphore_wait_timed(semaphore_t *sem);
void		semaphore_signal(semaphore_t *sem);

#endif /* !_LD_SEMAPHORE_H_ */
