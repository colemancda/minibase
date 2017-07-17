#include <sys/mmap.h>
#include <sys/file.h>
#include <string.h>
#include <null.h>

#include "config.h"
#include "svmon.h"

static char* ringarea;
static int ringlength;

static int extend_ring_area(void)
{
	int prot = PROT_READ | PROT_WRITE;
	int mflags = MAP_PRIVATE | MAP_ANONYMOUS;
	int rflags = MREMAP_MAYMOVE;

	int size = ringlength + PAGE;
	long new;

	if(ringarea)
		new = sys_mremap(ringarea, ringlength, size, rflags);
	else
		new = sys_mmap(NULL, size, prot, mflags, -1, 0);

	if(mmap_error(new)) {
		report("mmap failed", NULL, new);
		return new;
	}

	ringarea = (char*)new;
	ringlength = size;

	return 0;
}

static void index_ring_usage(short* use, int count)
{
	struct proc* rc;

	memzero(use, count*sizeof(*use));

	for(rc = firstrec(); rc; rc = nextrec(rc)) {
		if(!rc->ptr)
			continue;
		if(rc->idx < 0 || rc->idx >= count)
			continue;

		use[rc->idx] = 1 + (rc - procs);
	}

}

static struct proc* proc_by_idx(int i)
{
	if(i < 0 || i >= nprocs)
		return NULL;

	return &procs[i];
}

static char* ring_buf_by_idx(int i)
{
	int len = RINGSIZE;
	int off = len * i;
	int end = off + len;

	if(i < 0)
		return NULL;
	if(end > ringlength)
		return NULL;

	return ringarea + off;
}

char* ring_buf_for(struct proc* rc)
{
	return ring_buf_by_idx(rc->idx);
}

static void swap_ring_bufs(short* idx, int oldi, int newi)
{
	int ri = idx[oldi];
	struct proc* rc = proc_by_idx(ri - 1);

	char* old = ring_buf_by_idx(oldi);
	char* new = ring_buf_by_idx(newi);

	if(!rc || !old || !new)
		return;

	int len = rc->ptr > RINGSIZE ? RINGSIZE : rc->ptr;
	memcpy(new, old, len);
	rc->idx = newi;

	idx[newi] = ri;
	idx[oldi] = -1;
}

static void compact_used_bufs(short* idx, int count)
{
	int i = 0, j = count - 1;

	while(1) {
		while(i < j &&  idx[i]) i++;
		while(j > i && !idx[j]) j--;
		/* i = hole, j = last used */

		if(i < j)
			swap_ring_bufs(idx, j--, i++);
		if(i >= j)
			break;
	}
}

char* map_ring_buf(struct proc* rc)
{
	int i, count = ringlength / RINGSIZE;
	short idx[count];

	index_ring_usage(idx, count);
	compact_used_bufs(idx, count);

	for(i = 0; i < count; i++)
		if(!idx[i])
			break;
	if(i < count)
		;
	else if(extend_ring_area())
		return NULL;

	rc->idx = i;
	rc->ptr = 0;

	return ring_buf_for(rc);
}

int pages(int len)
{
	len += (PAGE - len % PAGE) % PAGE;
	return len / PAGE;
}

static void unmap_ring_area(void)
{
	if(sys_munmap(ringarea, ringlength) < 0)
		return;

	ringarea = NULL;
	ringlength = 0;
}

static void remap_ring_area(int count)
{
	int need = (count+1) * RINGSIZE;
	int havepages = pages(ringlength);
	int needpages = pages(need);

	if(needpages >= havepages)
		return;

	int size = needpages * PAGE;
	long new = sys_mremap(ringarea, ringlength, size, 0);

	if(mmap_error(new))
		return;

	ringarea = (char*)new;
	ringlength = size;
}

static int count_rings_needed(void)
{
	int count = ringlength / RINGSIZE;
	short idx[count];

	index_ring_usage(idx, count);
	compact_used_bufs(idx, count);

	while(count > 0 && !idx[count-1])
		count--;

	return count;
}

void trim_ring_area(void)
{
	int count = count_rings_needed();

	if(count)
		remap_ring_area(count);
	else
		unmap_ring_area();
}

void flush_ring_buf(struct proc* rc)
{
	if(!rc->ptr)
		return;

	rc->idx = -1;
	rc->ptr = 0;

	request(F_TRIM_RING);
}

static int wrapto(int x, int size)
{
	if(x > size)
		return size + x % size;
	else
		return x;
}

int read_into_ring_buf(struct proc* rc, int fd)
{
	int ptr = rc->ptr;
	char* buf;
	int ret;

	if(ptr)
		buf = ring_buf_for(rc);
	else
		buf = map_ring_buf(rc);
	if(!buf)
		return -ENOMEM;

	int off = ptr % RINGSIZE;
	char* start = buf + off;
	int avail = RINGSIZE - off;

	if((ret = sys_read(fd, start, avail)) <= 0)
		return ret;

	rc->ptr = wrapto(ptr + ret, RINGSIZE);

	return ret;
}
