#include "stdafx.h"
#include "common.h"

#include "event/event_epoll.h"
#include "event.h"

EVENT *event_create(int size)
{
	EVENT *ev = event_epoll_create(size);

	ring_init(&ev->events);
	ev->timeout = -1;
	ev->setsize = size;
	ev->maxfd   = -1;

	ring_init(&ev->poll_list);
	ring_init(&ev->epoll_list);

	return ev;
}

const char *event_name(EVENT *ev)
{
	return ev->name();
}

int event_handle(EVENT *ev)
{
	return ev->handle(ev);
}

int event_size(EVENT *ev)
{
	return ev->setsize;
}

void event_free(EVENT *ev)
{
	ev->free(ev);
}

#if 0
static int check_fdtype(int fd)
{
	struct stat s;

	if (fstat(fd, &s) < 0) {
		msg_info("%s(%d), %s: fd: %d fstat error %s", __FILE__,
			__LINE__, __FUNCTION__, fd, last_serror());
		return -1;
	}

	if (S_ISSOCK(s.st_mode)) {
		return 0;
	}
	if (S_ISFIFO(s.st_mode)) {
		return 0;
	}

	if (S_ISCHR(s.st_mode)) {
		return 0;
	}
	if (isatty(fd)) {
		return 0;
	}

	return -1;
}
#else
static int check_read_wait(EVENT *ev, FILE_EVENT *fe)
{
	if (ev->add_read(ev, fe) == -1) {
		fe->type = TYPE_NOSOCK;
		return -1;
	}

	if (ev->del_read(ev, fe) == -1) {
		fe->type = TYPE_NOSOCK;
		msg_error("%s(%d): del_read failed, fd=%d",
			__FUNCTION__, __LINE__, fe->fd);
		return -1;
	}

	fe->type = TYPE_SOCK;
	return 0;
}

static int check_write_wait(EVENT *ev, FILE_EVENT *fe)
{
	if (ev->add_write(ev, fe) == -1) {
		fe->type = TYPE_NOSOCK;
		return -1;
	}

	if (ev->del_write(ev, fe) == -1) {
		fe->type = TYPE_NOSOCK;
		msg_error("%s(%d): del_write failed, fd=%d",
			__FUNCTION__, __LINE__, fe->fd);
		return -1;
	}

	fe->type = TYPE_SOCK;
	return 0;
}
#endif

int event_add_read(EVENT *ev, FILE_EVENT *fe, event_proc *proc)
{
	assert(fe);

	if (fe->type == TYPE_NOSOCK) {
		return 0;
	}

	if (fe->fd >= ev->setsize) {
		msg_error("fd: %d >= setsize: %d", fe->fd, ev->setsize);
		errno = ERANGE;
		return 0;
	}

	if (fe->oper & EVENT_DEL_READ) {
		fe->oper &= ~EVENT_DEL_READ;
	}

	if (!(fe->mask & EVENT_READ)) {
		if (fe->type == TYPE_NONE && check_read_wait(ev, fe) == -1) {
			return 0;
		}

		if (fe->me.parent == &fe->me) {
			ring_prepend(&ev->events, &fe->me);
		}

		fe->oper |= EVENT_ADD_READ;
	}

	fe->r_proc = proc;
	return 1;
}

int event_add_write(EVENT *ev, FILE_EVENT *fe, event_proc *proc)
{
	assert(fe);

	if (fe->fd >= ev->setsize) {
		msg_error("fd: %d >= setsize: %d", fe->fd, ev->setsize);
		errno = ERANGE;
		return 0;
	}

	if (fe->oper & EVENT_DEL_WRITE) {
		fe->oper &= ~EVENT_DEL_WRITE;
	}

	if (!(fe->mask & EVENT_WRITE)) {
		if (fe->type == TYPE_NONE && check_write_wait(ev, fe) == -1) {
			return 0;
		}

		if (fe->me.parent == &fe->me) {
			ring_prepend(&ev->events, &fe->me);
		}

		fe->oper |= EVENT_ADD_WRITE;
	}

	fe->w_proc = proc;
	return 1;
}

void event_del_read(EVENT *ev, FILE_EVENT *fe)
{
	assert(fe);

	if (fe->oper & EVENT_ADD_READ) {
		fe->oper &=~EVENT_ADD_READ;
	}

	if (fe->mask & EVENT_READ) {
		if (fe->me.parent == &fe->me) {
			ring_prepend(&ev->events, &fe->me);
		}

		fe->oper |= EVENT_DEL_READ;
	}

	fe->r_proc  = NULL;
}

void event_del_write(EVENT *ev, FILE_EVENT *fe)
{
	assert(fe);

	if (fe->oper & EVENT_ADD_WRITE) {
		fe->oper &= ~EVENT_ADD_WRITE;
	}

	if (fe->mask & EVENT_WRITE) {
		if (fe->me.parent == &fe->me) {
			ring_prepend(&ev->events, &fe->me);
		}

		fe->oper |= EVENT_DEL_WRITE;
	}

	fe->w_proc = NULL;
}

void event_close(EVENT *ev, FILE_EVENT *fe)
{
	if (fe->mask & EVENT_READ) {
		ev->del_read(ev, fe);
	}

	if (fe->mask & EVENT_WRITE) {
		ev->del_write(ev, fe);
	}

	/* when one fiber add read/write and del read/write by another fiber
	 * in one loop, the fe->mask maybe be 0 and the fiber's fe maybe been
	 * added into events task list
	 */
	if (fe->me.parent != &fe->me) {
		ring_detach(&fe->me);
	}
}

static void event_prepare(EVENT *ev)
{
	FILE_EVENT *fe;
	RING *next;

	while ((next = ring_first(&ev->events))) {
		fe = ring_to_appl(next, FILE_EVENT, me);
		if (fe->oper & EVENT_ADD_READ) {
			ev->add_read(ev, fe);
		}
		if (fe->oper & EVENT_ADD_WRITE) {
			ev->add_write(ev, fe);
		}
		if (fe->oper & EVENT_DEL_READ) {
			ev->del_read(ev, fe);
		}
		if (fe->oper & EVENT_DEL_WRITE) {
			ev->del_write(ev, fe);
		}

		ring_detach(next);
		fe->oper = 0;
	}

	ring_init(&ev->events);
}

#define TO_APPL	ring_to_appl

static inline void event_process_poll(EVENT *ev)
{
	while (1) {
		POLL_EVENT *pe;
		RING *head = ring_pop_head(&ev->poll_list);

		if (head == NULL) {
			break;
		}

		pe = TO_APPL(head, POLL_EVENT, me);
		pe->proc(ev, pe);
	}

	ring_init(&ev->poll_list);
}

static void event_process_epoll(EVENT *ev)
{
	while (1) {
		EPOLL_EVENT *ee;
		RING *head = ring_pop_head(&ev->epoll_list);
		if (head == NULL) {
			break;
		}
		ee = TO_APPL(head, EPOLL_EVENT, me);
		ee->proc(ev, ee);
	}
}

int event_process(EVENT *ev, int timeout)
{
	int ret;

	if (ev->timeout < 0) {
		if (timeout < 0) {
			timeout = 100;
		}
	} else if (timeout < 0) {
		timeout = ev->timeout;
	} else if (timeout > ev->timeout) {
		timeout = ev->timeout;
	}

	/* limit the event wait time just for fiber schedule exiting
	 * quickly when no tasks left
	 */
	if (timeout > 1000 || timeout <= 0) {
		timeout = 100;
	}

	event_prepare(ev);
	ret = ev->event_loop(ev, timeout);
	event_process_poll(ev);
	event_process_epoll(ev);

	return ret;
}
