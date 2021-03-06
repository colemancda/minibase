#include <bits/socket/netlink.h>
#include <sys/file.h>
#include <sys/proc.h>
#include <sys/ppoll.h>
#include <sys/creds.h>
#include <sys/ioctl.h>
#include <sys/fpath.h>
#include <sys/dents.h>
#include <sys/socket.h>
#include <sys/signal.h>
#include <sys/mman.h>

#include <format.h>
#include <string.h>
#include <util.h>
#include <main.h>

#include "common.h"
#include "udevmod.h"

/* This service does few things that require monitoring udev events:

     * modprobes MODALIAS-es
     * chmods/chowns device nodes
     * writes device id data for libinput
     * re-transmits udev events for userspace listeners

   Common code here walks through /sys to pick up pre-registered devices,
   and then switches to listening for udev events. */

ERRTAG("udevmod");

#define OPTS "s"
#define OPT_s (1<<0)

#define UDEV_MGRP_KERNEL   (1<<0)
#define UDEV_MGRP_LIBUDEV  (1<<1)

/* Several events often come at once, so whatever files have been loaded
   should preferably be kept through the processing of the whole bunch.
   However, keeping them in memory for long is not good.

   So with any files loaded, do a timed wait and if there are no events
   for ~1s, unload them. */

static void unload(struct mbuf* mb)
{
	if(!mb->buf) return;

	sys_munmap(mb->buf, mb->len);

	mb->buf = NULL;
	mb->len = 0;
}

static int notempty(struct mbuf* mb)
{
	return !!(mb->buf);
}

static void wait_drop_files(CTX, int fd)
{
	struct pollfd pfd = { .fd = fd, .events = POLLIN };
	struct timespec ts = { 1, 0 };
	int ret;

	if(notempty(&ctx->config))
		;
	else if(notempty(&ctx->passwd))
		;
	else if(notempty(&ctx->group))
		;
	else return; /* no need to wait */

	if((ret = sys_ppoll(&pfd, 1, &ts, NULL)) != 0)
		return;

	unload(&ctx->config);
	unload(&ctx->passwd);
	unload(&ctx->group);
}

/* No point in doing a single-pass parsing for the events, it's easy
   enough to just pick the values as needed. Key here is something
   like MODALIAS or DEVPATH. */

char* getval(CTX, char* key)
{
	char* buf = ctx->uevent;
	char* end = buf + ctx->sep;

	char* p = buf;
	char* q;

	int klen = strlen(key);

	while(p < end) {
		if(!(q = strchr(p, '=')))
			goto next;
		if(q - p < klen)
			goto next;

		if(!strncmp(p, key, klen))
			return q + 1;

		next: p += strlen(p) + 1;
	}

	return NULL;
}

/* Generally udev events either carry a MODALIAS (unknown device, asking
   for a module) or have DEVNAME (device has been picked up by a module).
   Apparently it cannot be both at the same time. There are however events
   without MODALIAS and without DEVNAME. */

static void dev_added(CTX)
{
	char *alias, *subsystem, *devname;

	if((alias = getval(ctx, "MODALIAS")))
		modprobe(ctx, alias);
	if(!(subsystem = getval(ctx, "SUBSYSTEM")))
		return;
	if((devname = getval(ctx, "DEVNAME")))
		trychown(ctx, subsystem, devname);

	if(ctx->startup)
		return;
	if(!strcmp(subsystem, "input"))
		probe_input(ctx);
}

static void dev_removed(CTX)
{
	char* subsystem = getval(ctx, "SUBSYSTEM");

	if(ctx->startup)
		return;
	if(!strcmp(subsystem, "input"))
		clear_input(ctx);
}

/* Clients relying on udevd to modify device nodes anyhow subscribe to
   UDEV_MGRP_LIBUDEV not UDEV_MGRP_KERNEL and expect udevd to re-transmit
   the events once it's done processing them. This is necessary to avoid
   races between clients tryng to use the device and udevd trying to modify
   chmod/chown/whatever the node.

   This is mostly for conventional libudev clients.
   None of minibase tools rely on udevd to do anything about device nodes.

   The original udevd prepends a libudev-specific header to re-transmitted
   messages, but current libudev will happily accept raw kernel messages
   as well. Not sure whether it's intentional or not but it works. */

static void rebroadcast(CTX)
{
	int ret, fd = ctx->udev;

	char* buf = ctx->uevent;
	int len = ctx->ptr;

	struct sockaddr_nl addr = {
		.family = AF_NETLINK,
		.pad = 0,
		.pid = ctx->pid,
		.groups = UDEV_MGRP_LIBUDEV
	};

	if(ctx->startup)
		return;

	if((ret = sys_sendto(fd, buf, len, 0, &addr, sizeof(addr))) >= 0)
		return;
	if(ret == -ECONNREFUSED)
		return; /* this is ok apparently */

	warn("send", NULL, ret);
}

static void recv_event(CTX)
{
	int rd, fd = ctx->udev;
	int max = sizeof(ctx->uevent) - 2;
	char* buf = ctx->uevent;

	wait_drop_files(ctx, fd);

	if((rd = sys_recv(fd, buf, max, 0)) < 0)
		fail("recv", "udev", rd);

	buf[rd] = '\0';

	ctx->sep = rd;
	ctx->ptr = rd;

	if(!strncmp(buf, "remove@", 7))
		dev_removed(ctx);
	else if(!strncmp(buf, "add@", 4))
		dev_added(ctx);

	rebroadcast(ctx);
}

static void open_udev(CTX)
{
	int fd, ret;
	int pid = sys_getpid();

	int domain = PF_NETLINK;
	int type = SOCK_DGRAM;
	int proto = NETLINK_KOBJECT_UEVENT;

	if((fd = sys_socket(domain, type, proto)) < 0)
		fail("socket", "udev", fd);

	struct sockaddr_nl addr = {
		.family = AF_NETLINK,
		.pid = pid,
		.groups = UDEV_MGRP_KERNEL
	};

	if((ret = sys_bind(fd, &addr, sizeof(addr))) < 0)
		fail("bind", "udev", ret);

	ctx->pid = pid;
	ctx->udev = fd;
}

/* During startup, modaliases for pre-existing devices are picked
   up by scanning /sys/devices recursively and reading "modalias"
   files there. */

static void pick_modalias(CTX, FN)
{
	int fd, rd;
	char buf[100];
	int len = sizeof(buf) - 1;

	if((fd = sys_openat(AT(fn), O_RDONLY)) < 0)
		return;

	if((rd = sys_read(fd, buf, len)) > 0) {
		if(buf[rd-1] == '\n')
			buf[rd-1] = '\0';
		else
			buf[rd] = '\0';

		modprobe(ctx, buf);
	}

	sys_close(fd);
}

static int pathlen(FN)
{
	int len = strlen(fn->name);

	if(fn->dir && fn->name[0] != '/')
		len += strlen(fn->dir) + 1;

	return len + 1;
}

static void makepath(char* buf, int len, FN)
{
	char* p = buf;
	char* e = buf + len - 1;

	if(fn->dir && fn->name[0] != '/') {
		p = fmtstr(p, e, fn->dir);
		p = fmtstr(p, e, "/");
	};

	p = fmtstr(p, e, fn->name);
	*p = '\0';
}

static void scan_dir(CTX, FN)
{
	int len = 1024;
	char buf[len];
	int fd, rd;

	if((fd = sys_openat(AT(fn), O_DIRECTORY)) < 0)
		return;

	char path[pathlen(fn)];
	makepath(path, sizeof(path), fn);
	struct rfn next = { fd, path, NULL };

	while((rd = sys_getdents(fd, buf, len)) > 0) {
		char* ptr = buf;
		char* end = buf + rd;
		while(ptr < end) {
			struct dirent* de = (struct dirent*) ptr;

			if(!de->reclen)
				break;

			ptr += de->reclen;
			next.name = de->name;

			if(dotddot(de->name))
				continue;
			if(de->type == DT_DIR)
				scan_dir(ctx, &next);
			else if(de->type != DT_REG)
				continue;
			if(!strcmp(de->name, "modalias"))
				pick_modalias(ctx, &next);
		}
	}

	sys_close(fd);
}

static void scan_devices(CTX)
{
	struct rfn start = { AT_FDCWD, NULL, "/sys/devices" };

	scan_dir(ctx, &start);
}

/* modprobe running in pipe mode may die. That's not good, may
   be that pipe mode not supported, but it should not kill udevmod. */

static void suppress_sigpipe(void)
{
	SIGHANDLER(sa, SIG_IGN, 0);

	sys_sigaction(SIGPIPE, &sa, NULL);
}

int main(int argc, char** argv)
{
	struct top context, *ctx = &context;
	int i = 1, opts = 0;

	if(i < argc && argv[i][0] == '-')
		opts = argbits(OPTS, argv[i++] + 1);
	if(i < argc)
		fail("too many arguments", NULL, 0);

	memzero(ctx, sizeof(*ctx));

	ctx->envp = argv + argc + 1;
	ctx->opts = opts;
	ctx->startup = (opts & OPT_s);

	open_udev(ctx);

	suppress_sigpipe();
	open_modprobe(ctx);
	scan_devices(ctx);
	stop_modprobe(ctx);

	init_inputs(ctx);

	while(1) recv_event(ctx);
}
