#ifndef __SYSCALL2_H__
#define __SYSCALL2_H__

inline static long syscall2(int nr, long a1, long a2)
{
	register long r7 asm("r7") = nr;
	register long r0 asm("r0") = a1;
	register long r1 asm("r1") = a2;

	asm volatile ("svc 0" : "=r"(r0)
		: "r"(r7), "r"(r0), "r"(r1));
	
	return r0;
}

#endif
