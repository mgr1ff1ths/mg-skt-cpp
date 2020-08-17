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

	"mg-skt" non-blocking sockets abstraction layer.

 */

#include <stdio.h>
#include <sys/socket.h>
#include <stdint.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include "mg-skt.h"
#include "mg-skt_poll.h"
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <iostream>

using namespace std;

/* OSX compatibilty */
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK O_NONBLOCK
#endif
#ifndef SO_RCVBUFFORCE
#define SO_RCVBUFFORCE SO_RCVBUF
#endif
#ifndef SO_SNDBUFFORCE
#define SO_SNDBUFFORCE SO_SNDBUF
#endif

#define MG_RX_BUF_SIZE   5000
#define MG_TXQ_ENTRY_MAX 4

/*
 * mg_poll_drv_list contains the list of name / poll driver mappings,
 * populated on startup by constructors in global classes via mg_register()
 * e.g. for epoll and select.
 */
static unordered_map<string, mg_skt_poll_drv*> mg_poll_drv_list;

void mg_register(string name, mg_skt_poll_drv *drv)
{
	mg_poll_drv_list[name] = drv;
}

class mg_timer_cb {
private:
public:
	void *handle;
	void (*callback)(void*);
};

/* TODO: Can't seem to get queue container working correctly */
#define TXQ_ORIGINAL 1

#if TXQ_ORIGINAL
typedef struct {
	unsigned char *buf, *bufptr;
	int buflen;
} txq_entry_t;
#else
class txq_entry {
public:
	unsigned char *buf, *bufptr;
	int buflen;
	txq_entry(unsigned char *buf_, int buflen_)
	{
		buf = (unsigned char*)malloc(buflen_);
		assert(buf);
		bufptr = buf_;
		buflen = buflen_;
		memcpy(buf, bufptr, buflen);
	}
	~txq_entry(void)
	{
		free(buf);
		buf = NULL;
	}
};
#endif // TXQ_ORIGINAL
/* global structure */
class mg {
public:
	void *console_handle;
	unordered_set<class mg_timer_cb*> timer_cb_list;
	struct timeval timeout;
	mg_skt_poll_drv *poll_drv;
	mg(void) {
		timeout.tv_sec = 1;
	};
	int fd_add(int fd, class mg_skt *mg_skt)
	{
		return poll_drv->fd_add(fd, mg_skt);
	}
	int fd_del(class mg_skt *mg_skt)
	{
		return poll_drv->fd_del(mg_skt);
	}
	void *fd_open(int fd, mg_skt_param_t *p);
};

class mg_skt {
private:
	class mg *_mg;
public:
	mg_skt(class mg *mg) { _mg = mg; }
	void fd_tx_watch(int enable)
	{
		_mg->poll_drv->fd_tx_watch(this, enable);
	}
	void *fd_open(int fd, mg_skt_param_t *p)
	{
		return _mg->fd_open(fd, p);
	}
	int write_buf(unsigned char **buf, int *buflen)
	{
		fd_tx_watch(0);
		while (*buflen) {
			int l;
			if ((l = write(fd, *buf, *buflen)) < 0) {
				if (errno == EAGAIN) {
					/* write buffer full, enqueue the rest */
					fd_tx_watch(1);
					MG_LOG_DBG("mg_skt_write[%d]: tx_watch = 1\n", fd);
					break;
				}
				MG_LOG_ERR("mg_skt_write[%d]: write failed <%s>\n", fd, strerror(errno));
				assert(0);
			}
			else {
				MG_LOG_DBG("mg_skt_write[%d]: wrote %d of %d bytes\n", fd, l, *buflen);
				*buf += l;
				*buflen -= l;
			}
		}
		/* return the number of unwritten bytes */
		return *buflen;
	}
	int fd_add(int fd)
	{
		return _mg->poll_drv->fd_add(fd, this);
	}
	int fd_del()
	{
		return _mg->poll_drv->fd_del(this);
	}
	void skt_close()
	{
#if TXQ_ORIGINAL
		assert(!txq_entries());
#else
		assert(txq.empty());
#endif // TXQ_ORIGINAL
		fd_del();
		close(fd);
		delete this;
	}
	union {
		mg_skt_param_t		skt;
		mg_listen_param_t	listen;
	} params;
	int fd;
	void (*rx)(class mg_skt*);
#if TXQ_ORIGINAL
	int txq_s = 0;
	int txq_e = 0;
	txq_entry_t txq[MG_TXQ_ENTRY_MAX] = {};
	int txq_entries()
	{
		return (txq_s - txq_e + MG_TXQ_ENTRY_MAX) % MG_TXQ_ENTRY_MAX;
	}
#else
	queue<class txq_entry*> txq;
#endif // TXQ_ORIGINAL
};

void mg_rx(class mg_skt *mg_skt)
{
	assert(mg_skt->rx);
	mg_skt->rx(mg_skt);
}

void mg_dequeue(class mg_skt *mg_skt)
{
#if TXQ_ORIGINAL
	MG_LOG_DBG("mg_dequeue[%d]: %d entries\n", mg_skt->fd, mg_skt->txq_entries());
//	assert(mg_skt->txq_s != mg_skt->txq_e);
	while (mg_skt->txq_s != mg_skt->txq_e) {
		/* something to dequeue */
		txq_entry_t *txq = &mg_skt->txq[mg_skt->txq_s];
		unsigned char *bufptr = txq->bufptr;
		int buflen = txq->buflen;
		mg_skt->write_buf(&bufptr, &buflen);
		if (buflen == 0) {
			/* all data sent successfully, dequeue item */
			free(txq->buf);
			txq->buf = NULL;
			txq->bufptr = NULL;
			if (++mg_skt->txq_s == MG_TXQ_ENTRY_MAX) {
				/* wrap */
				mg_skt->txq_s = 0;
			}
		}
		else {
			/* not all the data was sent, save the rest */
			txq->bufptr = bufptr;
			txq->buflen = buflen;
			break;
		}
	}
	if (mg_skt->txq_s == mg_skt->txq_e) {
		/* all items have been dequeued */
		mg_skt->fd_tx_watch(0);
	}
#else
	MG_LOG_DBG("mg_dequeue[%d]: %lu entries\n", mg_skt->fd, mg_skt->txq.size());
	while (!mg_skt->txq.empty()) {
		/* something to dequeue */
		class txq_entry *entry = mg_skt->txq.front();
		std::cout << "mg_dequeue[" << mg_skt->fd << "]: entry = " << entry << " size = " << entry->buflen << '\n';
		unsigned char *bufptr = entry->bufptr;
		int buflen = entry->buflen;
		mg_skt->write(&bufptr, &buflen);
		if (buflen == 0) {
			/* all data sent successfully, dequeue item */
			mg_skt->txq.pop();
			delete entry;
		}
		else {
			/* not all the data was sent, save the rest */
			entry->bufptr = bufptr;
			entry->buflen = buflen;
			break;
		}
	}
#endif // TXQ_ORIGINAL
}

void mg_timeout(class mg *mg)
{
	MG_LOG_DBG("mg_timeout\n");
	for (class mg_timer_cb *t : mg->timer_cb_list) {
		t->callback(t->handle);
	}
}

static int mg_enqueue(class mg_skt *mg_skt, unsigned char *bufptr, int buflen)
{
#if TXQ_ORIGINAL
	MG_LOG_DBG("mg_enqueue[%d]: buflen = %d\n", mg_skt->fd, buflen);
	int qe = mg_skt->txq_e;
	int qe_next = qe + 1;
	txq_entry_t *txq = &mg_skt->txq[qe];
	if (qe_next == MG_TXQ_ENTRY_MAX) {
		qe_next = 0;
	}
	if (qe_next == mg_skt->txq_s) {
		MG_LOG_DBG("mg_enqueue[%d]: queue is full\n", mg_skt->fd);
		return -1;	// full
	}
	if ((txq->buf = (unsigned char*)malloc(buflen))) {
		memcpy(txq->buf, bufptr, buflen);
		txq->bufptr = txq->buf;
		txq->buflen = buflen;
		mg_skt->txq_e = qe_next;
		return 0;
	}
	assert(0);
	return -1;
#else
	MG_LOG_DBG("mg_enqueue[%d]: queue size = %lu\n", mg_skt->fd, mg_skt->txq.size());
	if (mg_skt->txq.size() >= MG_TXQ_ENTRY_MAX) {
		MG_LOG_DBG("mg_enqueue[%d]: queue is full\n", mg_skt->fd);
		assert(0);
		return -1;	// full
	}
	class txq_entry *entry = new txq_entry(bufptr, buflen);
	mg_skt->txq.push(entry);
	return 0;
#endif // TXQ_ORIGINAL
}

int mg_skt_tx(void *handle, unsigned char *bufptr, int buflen)
{
	class mg_skt *mg_skt = (class mg_skt*)handle;
	MG_LOG_DBG("mg_skt_tx[%d]: sending %d bytes\n", mg_skt->fd, buflen);
#if TXQ_ORIGINAL
	if (!mg_skt->txq_entries())
#else
	if (mg_skt->txq.empty())
#endif // TXQ_ORIGINAL
	{
		/* currently nothing enqueued, send it straight out */
		mg_skt->write_buf(&bufptr, &buflen);
	}
	if (buflen) {
		/* queue remaining data */
		MG_LOG_DBG("mg_skt_tx[%d]: enqueued %d bytes\n", mg_skt->fd, buflen);
		return mg_enqueue(mg_skt, bufptr, buflen);
	}
	return 0;
}

static void mg_skt_rx(class mg_skt *mg_skt)
{
	struct sockaddr_storage addr;
	socklen_t slen = sizeof(addr);
	unsigned char rx_buf[MG_RX_BUF_SIZE];
	int l = recvfrom(mg_skt->fd, rx_buf, sizeof(rx_buf),
	                 0, (struct sockaddr*)&addr, &slen);
	mg_skt_param_t *p = &mg_skt->params.skt;
	MG_LOG_DBG("mg_skt_rx[%d]: receiving %d bytes\n", mg_skt->fd, l);
	switch (l) {
	case -1:
		MG_LOG_ERR("mg_skt_rx: read failed <%s>\n", strerror(errno));
	/* drop through */
	case 0:
		/*  connection is closed */
		if (p->close) {
			p->close(p->handle);
		}
		mg_skt->skt_close();
		mg_skt = NULL;
		break;
	default:
		p->rx(p->handle, (struct sockaddr*)&addr, rx_buf, l);
		break;
	}
}

static void mg_read(class mg_skt *mg_skt)
{
	unsigned char rx_buf[MG_RX_BUF_SIZE];
	int l = read(mg_skt->fd, rx_buf, sizeof(rx_buf) - 1);
	if (l < 0) {
		MG_LOG_ERR("mg_skt_rx[%d]: read failed <%s>\n", mg_skt->fd, strerror(errno));
	}
	else {
		mg_skt_param_t *p = &mg_skt->params.skt;
		if (l > 0) {
			rx_buf[l] = 0;
			p->rx(p->handle, NULL, rx_buf, l);
		}
	}
}

void *mg_base::skt_open(mg_skt_param_t *p)
{
	class mg_skt *skt = new mg_skt((class mg*)priv);
	int r, on = 1;
	assert(skt);
	skt->rx = mg_skt_rx;
	if ((skt->fd = socket(p->family, p->type | SOCK_NONBLOCK, p->protocol)) < 0) {
		MG_LOG_ERR("mg_skt_open: socket failed <%s>\n", strerror(errno));
		assert(0);
	};
	MG_LOG_DBG("mg_skt_open: opening socket %d\n", skt->fd);
	r = setsockopt(skt->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	assert(r == 0);
	if (p->tx_buf_size) {
		r = setsockopt(skt->fd, SOL_SOCKET, SO_SNDBUFFORCE,
		               &p->tx_buf_size, sizeof(p->tx_buf_size));
		assert(r == 0);
	}
	if (p->rx_buf_size) {
		r = setsockopt(skt->fd, SOL_SOCKET, SO_RCVBUFFORCE,
		               &p->rx_buf_size, sizeof(p->rx_buf_size));
		assert(r == 0);
	}
	if (p->sock_addr && bind(skt->fd, p->sock_addr, p->slen) < 0) {
		MG_LOG_ERR("mg_skt_open: bind failed <%s>\n", strerror(errno));
		assert(0);
	}
	skt->params.skt = *p;
	skt->fd_add(skt->fd);
	if (p->connect_addr &&
	        connect(skt->fd, p->connect_addr, p->connect_addr_len) < 0) {
		switch (errno) {
		case EAGAIN:
		case EINPROGRESS:
			/* connection setup in progress */
			skt->fd_tx_watch(1);
			break;
		default:
			MG_LOG_ERR("mg_skt_open[%d]: connect failed <%s>\n",
			           skt->fd, strerror(errno));
			assert(0);
			break;
		}
	}
	else {
		MG_LOG_DBG("mg_skt_open[%d]: connect OK\n", skt->fd);
	}
	return (void*)skt;
}

void mg_base::skt_close(void *handle)
{
//	class mg *_mg = (class mg*)priv;
	class mg_skt *mg_skt = (class mg_skt*)handle;
#if TXQ_ORIGINAL
	assert(!mg_skt->txq_entries());
#else
	assert(mg_skt->txq.empty());
#endif // TXQ_ORIGINAL
	mg_skt->fd_del();
	close(mg_skt->fd);
	delete mg_skt;
}

int mg_skt_fd(void *handle)
{
	class mg_skt *skt = (class mg_skt*)handle;
	return skt->fd;
}

void *mg::fd_open(int fd, mg_skt_param_t *p)
{
	class mg_skt *skt = new mg_skt(this);
	int r, on = 1;
	assert(skt);
	skt->fd = fd;
	MG_LOG_DBG("mg_fd_open: opening socket %d\n", skt->fd);
	assert(skt->fd >= 0);
	skt->params.skt.rx = p->rx;
	skt->params.skt.close = p->close;
	skt->params.skt.handle = p->handle;
	if (p->sock_addr) {
		skt->rx = mg_skt_rx;
		r = setsockopt(skt->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		assert(r == 0);
		if (p->tx_buf_size) {
			r = setsockopt(skt->fd, SOL_SOCKET, SO_SNDBUFFORCE,
			               &p->tx_buf_size, sizeof(p->tx_buf_size));
			assert(r == 0);
		}
		if (p->rx_buf_size) {
			r = setsockopt(skt->fd, SOL_SOCKET, SO_RCVBUFFORCE,
			               &p->rx_buf_size, sizeof(p->rx_buf_size));
			assert(r == 0);
		}
	}
	else {
		skt->rx = mg_read;
	}
	skt->fd_add(skt->fd);
	return (void*)skt;
}

static void mg_accept(class mg_skt *mg_skt)
{
	struct sockaddr_storage addr_;
	struct sockaddr *addr = (struct sockaddr*)&addr_;
	socklen_t addr_len = sizeof(struct sockaddr_storage);
	int fd = accept(mg_skt->fd, addr, &addr_len);
	if (fd < 0) {
		MG_LOG_ERR("mg_accept[%d]: accept failed <%s>\n",
		           mg_skt->fd, strerror(errno));
		assert(0);
	}
	else {
		mg_listen_param_t *lp = &mg_skt->params.listen;
		void **client_handle;
		mg_skt_param_t p = {};
		p.sock_addr = addr;
		if ((client_handle = lp->accept(lp->handle, &p))) {
			*client_handle = mg_skt->fd_open(fd, &p);
		}
	}
}

mg_base::mg_base(void)
{
	priv = (void*)new(class mg);
}

int mg_base::init(std::string poll_drv_name)
{
	/* TODO: replace with find() or at() */
	for (auto &it : mg_poll_drv_list) {
		if (poll_drv_name == it.first) {
			/* found a match */
			class mg *_mg = (class mg*)priv;
			_mg->poll_drv = it.second;
			_mg->poll_drv->init(_mg);
			break;
		}
	}
	return 0;
}

int mg_base::dispatch(mg_param_t *p)
{
	int err = 0;
	class mg *_mg = (class mg*)priv;
	if (p && p->console.rx) {
		/* std_in support requested */
		mg_skt_param_t pc = {
			.handle = p->console.handle,
			.rx = p->console.rx
		};
		_mg->console_handle = _mg->fd_open(fileno(stdin), &pc);
	}
	while (!err) {
		err = _mg->poll_drv->wait_for_events();
	}
	return err;
}

void *mg_base::listen_open(mg_listen_param_t *p)
{
	class mg *_mg = (class mg*)priv;
	class mg_skt *skt = new mg_skt(_mg);
	int r, on = 1;
	assert(skt);
	assert(p->accept);
	skt->rx = mg_accept;
	skt->fd = socket(p->family, p->type, p->protocol);
	assert(skt->fd >= 0);
	r = setsockopt(skt->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	assert(r == 0);
	if (bind(skt->fd, p->sock_addr, p->slen) < 0) {
		MG_LOG_ERR("mg_listen_open: bind failed <%s>\n", strerror(errno));
		assert(0);
	}
	skt->params.listen = *p;
	_mg->fd_add(skt->fd, skt);
	MG_LOG_DBG("mg_listen_open: opening socket %d\n", skt->fd);
	r = listen(skt->fd, 10);	// 10 is an arbitrary queue length
	assert(r == 0);
	return (void*)skt;
}

void mg_base::listen_close(void *handle)
{
	class mg *_mg = (class mg*)priv;
	class mg_skt *mg_skt = (class mg_skt*)handle;
	_mg->fd_del(mg_skt);
	close(mg_skt->fd);
	delete mg_skt;
}

void *mg_base::timer_add(void *handle, void (*callback)(void*))
{
	class mg *_mg = (class mg*)priv;
	class mg_timer_cb *t = new mg_timer_cb();
	_mg->timer_cb_list.insert(t);
	t->callback = callback;
	t->handle = handle;
	return (void*)t;
}

void mg_base::timer_del(void *handle)
{
	class mg *_mg = (class mg*)priv;
	class mg_timer_cb *t = (class mg_timer_cb*)handle;
	_mg->timer_cb_list.erase(t);
	delete t;
}
