#include <bits/syscall.h>
#include <syscall2.h>

inline static long sysgetcwd(char* buf, unsigned long size)
{
	return syscall2(__NR_getcwd, (long) buf, size);
}
