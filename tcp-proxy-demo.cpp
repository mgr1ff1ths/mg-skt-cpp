/*

    COPYRIGHT AND PERMISSION NOTICE
    Copyright (c) 2019-2020 Mark Griffiths
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

    Simple single-threaded, multi-connect non-blocking TCP Proxy application
    demonstrating usage of mg-skt non-blocking library.

	Listens to port 8080 on local machine and connects to port 80 on remote
	machine.

 */

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <iostream>
#include <string>
#include <unordered_set>

#include <arpa/inet.h>
#include <assert.h>
#include "mg-skt.h"


#define HP_DATA_CONN_MAX 256

/* data connection record */
class tp_sock_data {
public:
	class tp_conn *conn;
	void *sock;
};

/* tcp proxy record */
class tpc {
public:
	void *listen_handle;
	struct in_addr srv_ip_loc;
	struct in_addr srv_ip_rem;
	std::unordered_set<class tp_conn*> conn;
	tpc(const char *loc, const char *rem)
	{
		inet_pton(AF_INET, loc, &srv_ip_rem);
		inet_pton(AF_INET, rem, &srv_ip_loc);
	};
	void conn_list_print(void);
	class mg_base *mg;
};

/* connection record */
class tp_conn {
private:
public:
	class tpc *tp;
	class tp_sock_data client_sock_data;
	class tp_sock_data server_sock_data;
	struct {
		struct in_addr ip;
		in_port_t port;
	} client;
	int age;
	tp_conn(class tpc *tp_, struct sockaddr_in *a)
	{
		printf("tp_conn constructor\n");
		tp = tp_;
		age = 0;
		client_sock_data.conn = this;
		server_sock_data.conn = this;
		client.ip.s_addr = a->sin_addr.s_addr;
		client.port = a->sin_port;
		tp->conn.insert(this);
	}
	~tp_conn(void)
	{
		printf("tp_conn destructor\n");
		tp->conn.erase(this);
	}
};

/* Print active client-server connections */
void tpc::conn_list_print(void)
{
	char ip_c[INET_ADDRSTRLEN];
	printf("--------------------------------\n");
	printf("|   Client IP    / Port  | Age |\n");
	printf("--------------------------------\n");
	for (tp_conn *c : conn) {
		inet_ntop(AF_INET, &c->client.ip, ip_c, sizeof(ip_c));
		printf("|%17s/%5d |%4d |\n", ip_c, c->client.port, c->age);
	}
	printf("--------------------------------\n");
}

/* Data received from the server -  send to the client */
static void tp_conn_server_rx(void *handle, struct sockaddr *rx_skt, unsigned char *buf, int buflen)
{
	class tp_sock_data *ds = (class tp_sock_data*)handle;
	tp_conn *c = ds->conn;
	class tp_sock_data *dc = &c->client_sock_data;
	if (mg_skt_tx(dc->sock, buf, buflen)) {
		printf("could not sent %d bytes from server to client\n", buflen);
	};
}

/* Data received from the client - send to the server */
static void tp_conn_client_rx(void *handle, struct sockaddr *rx_skt, unsigned char *buf, int buflen)
{
	class tp_sock_data *dc = (class tp_sock_data*)handle;
	tp_conn *c = dc->conn;
	class tp_sock_data *ds = &c->server_sock_data;
	if (mg_skt_tx(ds->sock, buf, buflen)) {
		printf("could not sent %d bytes from client to server\n", buflen);
	};
}

static void tp_conn_close(class tp_sock_data *d)
{
	d->conn->tp->mg->skt_close(d->sock);
//	mg_skt_close(d->sock);
	delete d->conn;
}

/* Client is closing the connection - close the server side */
static void tp_conn_client_close(void *handle)
{
	class tp_sock_data *dc = (class tp_sock_data*)handle;
	tp_conn_close(&dc->conn->server_sock_data);
}

/* Server is closing the connection - close the client side */
static void tp_conn_server_close(void *handle)
{
	class tp_sock_data *ds = (class tp_sock_data*)handle;
	tp_conn_close(&ds->conn->client_sock_data);
}

/* Process inbound connection request from client and make it to the server */
static void **tp_conn_accept(void *tp_conn_handle, mg_skt_param_t *cp)
{
	tpc *tp = (tpc*)tp_conn_handle;
	tp_conn *c = new tp_conn(tp, (struct sockaddr_in*)cp->sock_addr);
	if (c) {
		/* found a free data connection - open a data socket to the server */
		class tp_sock_data *dc = &c->client_sock_data;
		class tp_sock_data *ds = &c->server_sock_data;
		struct sockaddr_in connect_addr = {
			.sin_family = AF_INET,
			.sin_port = htons(80),
			.sin_addr = tp->srv_ip_rem,
		};
		mg_skt_param_t server_data_skt_param = {
			.handle = ds,
			.rx = tp_conn_server_rx,
			.close = tp_conn_server_close,
			.family = AF_INET,
			.type = SOCK_STREAM,
			.connect_addr = (struct sockaddr*)&connect_addr,
			.connect_addr_len = sizeof(connect_addr),
		};
		ds->conn = c;
		ds->sock = tp->mg->skt_open(&server_data_skt_param);
		assert(ds->sock);
		/* fill in client params */
		cp->handle = (void*)dc;
		cp->rx = tp_conn_client_rx;
		cp->close = tp_conn_client_close;
		dc->conn = c;
		/* return the *address* of the client's data connection handle */
		return &dc->sock;
	}
	return NULL;
}

/* Accept console input. Any input generates a list of active connections */
static void tp_console_rx(void *handle, struct sockaddr *rx_skt,
						  unsigned char *rx_buf, int rx_buflen)
{
	tpc *tp = (tpc*)handle;
	tp->conn_list_print();
}

/* 1 second timeout */
static void tp_timeout(void *handle)
{
	tpc *tp = (tpc*)handle;
	for (class tp_conn *c : tp->conn) {
		c->age++;
	}
}

int main(int argc, char *argv[])
{
	/* validate input */
	struct in_addr ip;
	if (argc != 3 ||
	        inet_pton(AF_INET, argv[1], &ip) != 1 ||
	        inet_pton(AF_INET, argv[2], &ip) != 1) {
		/* couldn't parse IP address(es) */
		printf("usage: tp <remote IPv4 address> <my IPv4 address>\n");
		exit(1);
	}
	/* construct tp object */
	tpc tp(argv[1], argv[2]);
	struct sockaddr_in listen_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(8080),
		.sin_addr = { .s_addr = tp.srv_ip_loc.s_addr }
	};
	mg_listen_param_t listen_param = {
		.handle = (void*)&tp,
		.accept = tp_conn_accept,
		.family = AF_INET,
		.type = SOCK_STREAM,
		.sock_addr = (struct sockaddr*)&listen_addr,
		.slen = sizeof(listen_addr)
	};
	/* initialize */
	mg_base *mg = tp.mg = new mg_base;
	mg->init("select");	// use select multiplexing
//	mg->init("epoll");	// use epoll multiplexing
	tp.listen_handle = mg->listen_open(&listen_param);
	assert(tp.listen_handle);
	/* allow console input */
	mg_param_t tpp = {
		.console = { .rx = tp_console_rx, .handle = &tp }
	};
	/* add a 1-sec periodic timer */
	mg->timer_add((void*) &tp, tp_timeout);
	/* start waiting for events */
	return mg->dispatch(&tpp);
}
