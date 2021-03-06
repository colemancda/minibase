#include <sys/file.h>
#include <sys/fprop.h>
#include <sys/mman.h>

#include <format.h>
#include <main.h>
#include <util.h>

ERRTAG("sparse");

static void skip(int fd, long len)
{
	off_t temp;

	sys_llseek(fd, len, &temp, SEEK_CUR);
}

static void fill(int fd, long len, int rn)
{
	char buf[len];
	int rd;

	if((rd = sys_read(rn, buf, len)) < 0)
		fail("read", NULL, rd);
	if(rd < len)
		fail("incomplete read", NULL, rd);

	writeall(fd, buf, len);
}

int main(int argc, char** argv)
{
	int fd, rn;
	char* name = argv[1];
	char* rand = "/dev/urandom";

	if(argc != 2)
		fail("bad call", NULL, 0);

	if((fd = sys_open3(name, O_WRONLY | O_CREAT | O_TRUNC, 0644)) < 0)
		fail(NULL, name, fd);

	if((rn = sys_open(rand, O_RDONLY)) < 0)
		fail(NULL, rand, rn);

	skip(fd, 10*PAGE);
	fill(fd, 5*PAGE, rn);
	skip(fd, 20*PAGE);
	fill(fd, 15*PAGE, rn);
	skip(fd, 1*PAGE);

	return 0;
}
