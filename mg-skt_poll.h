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

#ifndef __MG_SKT_POLL_H__
#define __MG_SKT_POLL_H__

#if (1)	// set to 1 for debug messages
#define MG_LOG_DBG(...) printf(__VA_ARGS__)
#else
#define MG_LOG_DBG(...)
#endif

#if (1)	// set to 1 for error messages
#define MG_LOG_ERR(...) printf(__VA_ARGS__)
#else
#define MG_LOG_ERR(...)
#endif

class mg_skt_poll_drv {
public:
    std::string name;
	virtual int init(class mg*) { return 0; };
	virtual int fd_add(int fd, class mg_skt*) { return 0; };
	virtual int fd_del(class mg_skt*) { return 0; };
	virtual int fd_tx_watch(class mg_skt*, int) { return 0; };
	virtual int wait_for_events(void) { return 0; };
};
void mg_register(std::string name, mg_skt_poll_drv *drv);
void mg_dequeue(class mg_skt*);
void mg_rx(class mg_skt*);
void mg_timeout(class mg*);

#endif // __MG_SKT_POLL_H__
