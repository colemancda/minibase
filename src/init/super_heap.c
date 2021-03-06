#include <sys/mman.h>

#include <cdefs.h>
#include "super.h"

/* Heap is only used for large buffers in load_dir_ents()
   and ctrl code. Custom routines because the ones from util.h
   rely on fail() atm and pid 1 should be careful with that. */

static void* brk;
static void* ptr;
static void* end;

void setup_heap(void)
{
	brk = sys_brk(NULL);
	ptr = end = brk;
}

static long alignto(long x, long size)
{
	return x + (size - x % size) % size;
}

void* heap_alloc(int len)
{
	void* old = ptr;
	void* new = old + len;
	void* req = old + alignto(len, PAGE);

	if(new > end)
		end = sys_brk(req);
	if(new > end)
		return NULL;

	ptr = new;
	return old;
}

void trim_heap(void* old)
{
	if(old < brk || old > end)
		return;

	ptr = old;
	request(F_FLUSH_HEAP);
}

void flush_heap(void)
{
	if(ptr > brk)
		report("retaining non-empty heap", NULL, 0);
	else if(end == brk)
		return;
	else
		end = sys_brk(brk);
}
