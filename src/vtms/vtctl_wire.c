#include <bits/socket/unix.h>

#include <sys/mman.h>
#include <sys/file.h>
#include <sys/socket.h>

#include <string.h>
#include <util.h>

#include "common.h"
#include "vtctl.h"

/* Socket init is split in two parts: socket() call is performed early so
   that it could be used to resolve netdev names into ifis, but connection
   is delayed until send_command() to avoid waking up wimon and then dropping
   the connection because of a local error. */

void init_socket(CTX)
{
	int fd;

	if((fd = sys_socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
		fail("socket", "AF_UNIX", fd);

	ctx->fd = fd;
}

static void connect_socket(CTX)
{
	int ret;

	struct sockaddr_un addr = {
		.family = AF_UNIX,
		.path = CONTROL
	};

	if((ret = sys_connect(ctx->fd, &addr, sizeof(addr))) < 0)
		fail("connect", addr.path, ret);

	ctx->connected = 1;
}

void start_request(CTX, int cmd)
{
	void* brk;
	int len = sizeof(ctx->recbuf);

	brk = ctx->recbuf;

	ctx->uc.brk = brk;
	ctx->uc.ptr = brk;
	ctx->uc.end = brk + len;

	uc_put_hdr(&ctx->uc, cmd);
}

void add_str_attr(CTX, int key, char* val)
{
	uc_put_str(&ctx->uc, key, val);
}

void add_int_attr(CTX, int key, int val)
{
	uc_put_int(&ctx->uc, key, val);
}

void send_request(CTX)
{
	int wr, fd = ctx->fd;
	char* txbuf = ctx->uc.brk;
	int txlen = ctx->uc.ptr - ctx->uc.brk;

	if(!txlen)
		fail("trying to send an empty message", NULL, 0);

	uc_put_end(&ctx->uc);

	if(!ctx->connected)
		connect_socket(ctx);

	if((wr = sys_send(fd, txbuf, txlen, 0)) < 0)
		fail("write", NULL, wr);
	else if(wr < txlen)
		fail("incomplete write", NULL, 0);

	memzero(&ctx->uc, sizeof(ctx->uc));
}

static void init_small_rxbuf(CTX)
{
	if(ctx->uc.brk == ctx->recbuf)
		fail("smallbuf tx-locked", NULL, 0);

	void* buf = ctx->recbuf;
	int len = sizeof(ctx->recbuf);

	ctx->ur.buf = buf;
	ctx->ur.mptr = buf;
	ctx->ur.rptr = buf;
	ctx->ur.end = buf + len;
}

struct ucmsg* recv_reply(CTX)
{
	int ret;

	if(!ctx->ur.buf)
		init_small_rxbuf(ctx);

	while((ret = uc_recv(ctx->fd, &ctx->ur, !0)) < 0)
		fail("recv", NULL, ret);

	return ctx->ur.msg;
}

void init_output(CTX)
{
	ctx->bo.fd = STDOUT;
	ctx->bo.buf = ctx->outbuf;
	ctx->bo.ptr = 0;
	ctx->bo.len = sizeof(ctx->outbuf);
}

void flush_output(CTX)
{
	if(!ctx->bo.ptr)
		return;

	bufoutflush(&ctx->bo);
}

void output(CTX, char* buf, int len)
{
	if(ctx->bo.len)
		bufout(&ctx->bo, buf, len);
	else
		writeall(STDOUT, buf, len);
}
