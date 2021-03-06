/*
 *  gensio - A library for abstracting stream I/O
 *  Copyright (C) 2018  Corey Minyard <minyard@acm.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307  USA
 */

#include "config.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <errno.h>
#include <gensio/waiter.h>

#ifdef USE_PTHREADS

#include <pthread.h>
#include <signal.h>
#include <stdbool.h>

struct waiter_timeout {
    struct timeval tv;
    struct waiter_timeout *prev;
    struct waiter_timeout *next;
};

struct waiter_s {
    struct selector_s *sel;
    int wake_sig;
    unsigned int count;
    pthread_mutex_t lock;
    struct waiter_timeout wts;
};

waiter_t *
alloc_waiter(struct selector_s *sel, int wake_sig)
{
    waiter_t *waiter;

    waiter = malloc(sizeof(waiter_t));
    if (waiter) {
	memset(waiter, 0, sizeof(*waiter));
	waiter->sel = sel;
	pthread_mutex_init(&waiter->lock, NULL);
	waiter->wts.next = &waiter->wts;
	waiter->wts.prev = &waiter->wts;
    }
    return waiter;
}

void
free_waiter(waiter_t *waiter)
{
    assert(waiter);
    /* assert(waiter->count == 0); */
    assert(waiter->wts.next == waiter->wts.prev);
    pthread_mutex_destroy(&waiter->lock);
    free(waiter);
}

struct wait_data {
    pthread_t id;
    int wake_sig;
};

static void
wake_thread_send_sig(long thread_id, void *cb_data)
{
    struct wait_data *w = cb_data;

    pthread_kill(w->id, w->wake_sig);
}

static int
i_wait_for_waiter_timeout(waiter_t *waiter, unsigned int count,
			  struct timeval *timeout, bool intr)
{
    struct waiter_timeout wt;
    struct wait_data w;
    int err = 0;

    w.id = pthread_self();
    w.wake_sig = waiter->wake_sig;

    wt.tv.tv_sec = LONG_MAX;
    wt.next = NULL;
    wt.prev = NULL;
    pthread_mutex_lock(&waiter->lock);

    waiter->wts.next->prev = &wt;
    wt.next = waiter->wts.next;
    waiter->wts.next = &wt;
    wt.prev = &waiter->wts;

    while (waiter->count < count) {
	pthread_mutex_unlock(&waiter->lock);
	if (intr)
	    err = sel_select_intr(waiter->sel, wake_thread_send_sig,
				  w.id, &w, timeout);
	else
	    err = sel_select(waiter->sel, wake_thread_send_sig, w.id, &w,
			     timeout);
	if (err < 0)
	    err = errno;
	else if (err == 0)
	    err = ETIMEDOUT;
	else
	    err = 0;
	/* lock may affect errno, delay it until here. */
	pthread_mutex_lock(&waiter->lock);
	if (err)
	    break;
    }
    if (!err)
	waiter->count -= count;
    wt.next->prev = wt.prev;
    wt.prev->next = wt.next;
    pthread_mutex_unlock(&waiter->lock);

    return err;
}

void
wake_waiter(waiter_t *waiter)
{
    struct waiter_timeout *wt;

    pthread_mutex_lock(&waiter->lock);
    waiter->count++;
    wt = waiter->wts.next;
    while (wt != &waiter->wts) {
	wt->tv.tv_sec = 0;
	wt = wt->next;
    }
    sel_wake_all(waiter->sel);
    pthread_mutex_unlock(&waiter->lock);
}

#else /* USE_PTHREADS */

struct waiter_s {
    unsigned int count;
    struct selector_s *sel;
};

waiter_t *
alloc_waiter(struct selector_s *sel, int wake_sig)
{
    waiter_t *waiter;

    waiter = malloc(sizeof(waiter_t));
    if (waiter)
	memset(waiter, 0, sizeof(*waiter));
    return waiter;
}

void
free_waiter(waiter_t *waiter)
{
    assert(waiter);
    assert(waiter->count == 0);
    free(waiter);
}

static int
i_wait_for_waiter_timeout(waiter_t *waiter, unsigned int count,
			  struct timeval *timeout, bool intr)
{
    int err;

    while (waiter->count < count) {
	if (intr)
	    err = sel_select_intr(waiter->sel, 0, 0, NULL, timeout);
	else
	    err = sel_select(waiter->sel, 0, 0, NULL, timeout);
	if (err < 0) {
	    err = errno;
	    break;
	} else if (err == 0) {
	    err = ETIMEDOUT;
	    break;
	}
	err = 0;
    }
    if (!err)
	waiter->count -= count;
    return err;
}

void
wake_waiter(waiter_t *waiter)
{
    waiter->count++;
}

#endif /* USE_PTHREADS */

int
wait_for_waiter_timeout(waiter_t *waiter, unsigned int count,
			struct timeval *timeout)
{
    return i_wait_for_waiter_timeout(waiter, count, timeout, false);
}

void
wait_for_waiter(waiter_t *waiter, unsigned int count)
{
    wait_for_waiter_timeout(waiter, count, NULL);
}

int
wait_for_waiter_timeout_intr(waiter_t *waiter, unsigned int count,
				 struct timeval *timeout)
{
    return i_wait_for_waiter_timeout(waiter, count, timeout, true);
}

int
wait_for_waiter_intr(waiter_t *waiter, unsigned int count)
{
    return wait_for_waiter_timeout_intr(waiter, count, NULL);
}
