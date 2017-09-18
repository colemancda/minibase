struct mbuf {
	char* buf;
	long len;
	long full;
};

struct line {
	char* ptr;
	char* sep;
	char* end;
};

struct top {
	int argc;
	int argi;
	int opts;

	char** argv;
	char** envp;

	void* brk;
	void* lwm;
	void* ptr;
	void* end;

	struct mbuf modules_dep;
	struct mbuf etc_modopts;
	int did_modopts;

	char** deps;

	char* release;
};

typedef char* (*lnmatch)(char* ls, char* le, char* tag, int len);

#define CTX struct top* ctx

void insmod(CTX, char* name, char* opts);
void prep_release(CTX);
void prep_modules_dep(CTX);
char** query_deps(CTX, char* name);
char* query_pars(CTX, char* name);

void mmap_whole(struct mbuf* mb, char* name, int strict);
void decompress(CTX, struct mbuf* mb, char* path, char* cmd);

void* heap_alloc(CTX, int size);
void unmap_buf(struct mbuf* mb);
void flush_heap(CTX);
