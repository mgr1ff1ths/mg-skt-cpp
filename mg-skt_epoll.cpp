/*

    COPYRIGHT AND PERMISSION NOTICE
    Copyright (c) 2015-2020 Mark Griffiths
    All rights reserved.
    Permission to use, copy, modify, and distribute this software for any
    purpose with or without fee is hereby granted, provided that the above
    copyright notice and this permission notice appear in all copies.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
    OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF THIRD PARTY RIGHTS.
    IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
    DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
    OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
    USE OR OTHER DEALINGS IN THE SOFTWARE.

    Except as contained in this notice, the name of a copyright holder shall
    not be used in advertising or otherwise to promote the sale, use or other
    dealings in this Software without prior written authorization of the
    copyright holder.

	"mg-skt" non-blocking sockets abstraction layer epoll module.

 */
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include "mg-skt.h"
#include "mg-skt_poll.h"
#include <string>

#define MAXEVENTS 64

class mg_skt_poll_epoll : mg_skt_poll_drv {
private:
	int timer_fd;
	int efd;
	class mg *_mg_handle;
public:
	std::string name = "epoll";
	// constructor
	mg_skt_poll_epoll()
	{
		mg_register(name, this);
	};
	// override init function
	int init(class mg *mg_handle)
	{
		struct itimerspec new_value = {
			.it_interval = { .tv_sec = 1 },
			.it_value = { .tv_sec = 1 }
		};
		efd = epoll_create1(0);
		MG_LOG_DBG("mg_epoll_init: efd = %d\n", efd);
		assert(efd >= 0);
		/* setup one sec recurring timer */
		timer_fd = timerfd_create(CLOCK_MONOTONIC, 0);
		assert(timer_fd >= 0);
		if (timerfd_settime(timer_fd, 0, &new_value, NULL) < 0) {
			assert(0);
		}
		if (fd_add(timer_fd, NULL)) {
			assert(0);
		}
		_mg_handle = mg_handle;
		return 0;
	}
	// add a file descriptor
	int fd_add(int fd, class mg_skt *mg_skt)
	{
		struct epoll_event event;
		event.data.ptr = (void*)mg_skt;
		event.events =  EPOLLIN | EPOLLET;
		if (epoll_ctl(efd, EPOLL_CTL_ADD, fd, &event)) {
			MG_LOG_ERR("fd_add: epoll_ctl failed <%s>, efd=%d, fd=%d\n",
			           strerror(errno), efd, fd);
			assert(0);
		}
		return 0;
	}
	// delete a file descriptor
	int fd_del(class mg_skt *mg_skt)
	{
		MG_LOG_DBG("mg_epoll_fd_del: fd = %d\n", mg_skt_fd(mg_skt));
		if (epoll_ctl(efd, EPOLL_CTL_DEL, mg_skt_fd(mg_skt), NULL)) {
			assert(0);
		}
		return 0;
	}
	// watch for tx complete event
	int fd_tx_watch(class mg_skt *mg_skt, int enable)
	{
		struct epoll_event event;
		event.data.ptr = (void*)mg_skt;
		event.events = enable ? (EPOLLIN | EPOLLOUT | EPOLLET) : (EPOLLIN | EPOLLET);
		if (epoll_ctl(efd, EPOLL_CTL_MOD, mg_skt_fd(mg_skt), &event)) {
			MG_LOG_ERR("mg_epoll_tx_watch: epoll_ctl failed <%s>, efd=%d, fd=%d\n",
			           strerror(errno), efd, mg_skt_fd(mg_skt));
			assert(0);
		}
		return 0;
	};
	// wait_for_events
	int wait_for_events(void)
	{
		struct epoll_event events[MAXEVENTS], *e;
		int i, err = 0;
		int n = epoll_wait(efd, events, MAXEVENTS, -1);
		if (n < 0) {
			if (errno == EINTR) {
				MG_LOG_DBG("wait_for_events: signal interrupt...resuming\n");
			}
			else {
				MG_LOG_ERR("wait_for_events: epoll_wait err %s\n", strerror(errno));
				err = errno;
				assert(0);
			}
			return 0;
		}
		for (i = 0, e = events; i < n; i++, e++) {
			if (e->events & EPOLLHUP) {
				continue;
			}
			assert((e->events & EPOLLERR) == 0);
			class mg_skt *mg_skt = (class mg_skt*)e->data.ptr;
			if (e->events & (EPOLLOUT)) {
				mg_dequeue(mg_skt);
			}
			if (e->events & (EPOLLIN)) {
				if (mg_skt) {
					mg_rx(mg_skt);
				}
				else {
					uint64_t exp;
					int s = read(timer_fd, &exp, sizeof(uint64_t));
					assert(s == sizeof(uint64_t));
					mg_timeout(_mg_handle);
				}
			}
		}
		return err;
	}
};

// create an instance of this: constructor calls mg_register()
static class mg_skt_poll_epoll mg_epoll;
