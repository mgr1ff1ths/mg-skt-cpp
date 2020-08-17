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

 */

#ifndef __MG_SKT_H__
#define __MG_SKT_H__

#include <string>

struct sockaddr;

typedef struct {
	void *handle;
	void (*rx)(void*, struct sockaddr*, unsigned char*, int);
	void (*close)(void*);
	int family;
	int type;
	struct sockaddr *connect_addr;
	socklen_t connect_addr_len;
	int protocol;
	struct sockaddr *sock_addr;
	socklen_t slen;
	uint32_t tx_buf_size;
	uint32_t rx_buf_size;
} mg_skt_param_t;

typedef struct {
	void *handle;
	void **(*accept)(void*, mg_skt_param_t*);
	int family;
	int type;
	struct sockaddr *sock_addr;
	socklen_t slen;
	int protocol;
} mg_listen_param_t;

typedef struct {
	struct {
		void (*rx)(void*, struct sockaddr *, unsigned char*, int);
		void *handle;
	} console;
} mg_param_t;

class mg_base {
public:
	mg_base();
	int init(std::string);
	int dispatch(mg_param_t*);
	void *listen_open(mg_listen_param_t *p);
	void listen_close(void*);
	void *skt_open(mg_skt_param_t *p);
	void skt_close(void*);
	void *timer_add(void *handle, void (*callback)(void*));
	void timer_del(void*);
private:
	/* hide all the private stuff here! */
	void *priv;
};

int mg_skt_tx(void *handle, unsigned char *buf, int len);
int mg_skt_fd(void *handle);

#endif // __MG_SKT_H__
