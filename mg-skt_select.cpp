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

	"mg-skt" non-blocking sockets abstraction layer select module.

 */

#include <sys/socket.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <assert.h>
#include "mg-skt.h"
#include "mg-skt_poll.h"
#include <unordered_map>

using namespace std;

#define MG_FD_LIST_SIZE  32

typedef struct mg_timer_cb {
	struct mg_timer_cb *next;
	void *handle;
	void (*callback)(void*);
} mg_timer_cb_t;

class mg_fd_list_entry {
public:
	class mg_skt *mg_skt;
	int tx_watch;
	int deleting;
	int tx_watch_timestamp;
	mg_fd_list_entry(class mg_skt *skt)
	{
		mg_skt = skt;
		tx_watch = 0;
		deleting = 0;
	}
};

class mg_skt_poll_select : mg_skt_poll_drv {
private:
	class mg *_mg_handle;
	mg_timer_cb_t *timer_cb_first;
	int fd_isset_in_progress;
	unordered_map<int, mg_fd_list_entry*> fd_list;
	struct timeval timeout;
	int mg_fd_set(fd_set *rx_fds, fd_set *tx_fds)
	{
		int max_fd = 0;
		FD_ZERO(rx_fds);
		FD_ZERO(tx_fds);
		for (auto it = fd_list.begin(); it != fd_list.end(); it++) {
			int fd = it->first;
			mg_fd_list_entry *le = it->second;
			assert(le->deleting == 0);
			FD_SET(fd, rx_fds);
			if (le->tx_watch) {
				printf("mg_fd_set[%d]: tx_watch\n", fd);
				FD_SET(fd, tx_fds);
				fcntl(fd, F_SETFL, (fcntl(fd, F_GETFL) | O_NONBLOCK));
			}
			if (fd > max_fd) {
				max_fd = fd;
			}
		}
		return max_fd;
	}
	int mg_fd_isset(fd_set *rx_fds, fd_set *tx_fds)
	{
		fd_isset_in_progress = 1;
		for (auto it = fd_list.begin(); it != fd_list.end(); it++) {
			int fd = it->first;
			mg_fd_list_entry *le = it->second;
			if (le->deleting) {
				FD_CLR(fd, tx_fds);
				FD_CLR(fd, rx_fds);
				continue;
			}
			if (FD_ISSET(fd, tx_fds)) {
				mg_dequeue(le->mg_skt);
				FD_CLR(fd, tx_fds);
			}
			if (FD_ISSET(fd, rx_fds)) {
				mg_rx(le->mg_skt);
				FD_CLR(fd, rx_fds);
			}
		}
		fd_isset_in_progress = 0;
		/* clean up deleted fds */
		for (auto it = fd_list.begin(), it_next = it; it != fd_list.end(); it = it_next) {
			it_next = next(it, 1);
			mg_fd_list_entry *le = it->second;
			if (le->deleting) {
				delete(it->second);
				fd_list.erase(it);
			}
		}
		return 0;
	}
public:
	const char *name = "select";
	// constructor
	mg_skt_poll_select()
	{
		mg_register(name, this);
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;
	};
	int init(class mg *mg_handle)
	{
		_mg_handle = mg_handle;
		return 0;
	}
	// add a file descriptor
	int fd_add(int fd, class mg_skt *skt)
	{
		assert(fd_list.size() < MG_FD_LIST_SIZE);
		fd_list.emplace(fd, new mg_fd_list_entry(skt));
		return 0;
	}
	int fd_del(class mg_skt *skt)
	{
		assert(fd_list.size());
		auto it = fd_list.find(mg_skt_fd(skt));
		assert(it != fd_list.end());
		if (fd_isset_in_progress) {
			mg_fd_list_entry *le = it->second;
			le->deleting = 1;
		}
		else {
			delete(it->second);
			fd_list.erase(it);
		}
		return 0;
	}
	// watch for tx complete event
	int fd_tx_watch(class mg_skt *mg_skt, int enable)
	{
		MG_LOG_DBG("fd_tx_watch [%d] enable = %d, count = %lu\n", mg_skt_fd(mg_skt), enable, fd_list.size());
		auto it = fd_list.find(mg_skt_fd(mg_skt));
		assert(it != fd_list.end());
		mg_fd_list_entry *le = it->second;
		le->tx_watch = enable;
		le->tx_watch_timestamp = enable ? time(NULL) : 0;
		return 0;
	};
	// wait_for_events
	int wait_for_events(void)
	{
		fd_set rx_fds, tx_fds;
		int err = 0;
		int max_fd = mg_fd_set(&rx_fds, &tx_fds);
		int n = select(max_fd + 1, &rx_fds, &tx_fds, NULL, &timeout);
		if (n < 0) {
			if (errno == EINTR) {
				MG_LOG_DBG("mg_poll: signal interrupt...resuming\n");
			}
			else {
				MG_LOG_ERR("mg_poll: select err %s\n", strerror(errno));
				err = errno;
				assert(0);
			}
		}
		else if (n > 0) {
			mg_fd_isset(&rx_fds, &tx_fds);
		}
		else {
			mg_timeout(_mg_handle);
			timeout.tv_sec = 1;
		}
		return err;
	}
};

// create an instance of this: constructor calls mg_register()
static class mg_skt_poll_select mg_select;
