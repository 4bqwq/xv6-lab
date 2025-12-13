// test-1: sys_getpid and sys_print
#include "sys.h"

int main()
{
	int pid = syscall(SYS_getpid);
	if (pid == 1) {
		syscall(SYS_print_str, "\nproczero: hello ");
		syscall(SYS_print_str, "world!\n");
	}
	while (1);	
}