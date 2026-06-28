/* Negative MPU-isolation test for the FreeRTOS-MPU Linux personality.
 *
 * An unprivileged Linux program deliberately writes to kernel SRAM (0x20000000), which lies
 * OUTSIDE its per-task MPU regions (its program region + dyn_pool). Under the ARM_CM4_MPU port
 * this raises a MemManage fault, which the personality contains: the program is killed like a
 * default-action SIGSEGV (exit 139) and the coordinator reaps it — the kernel and the shell are
 * untouched. A working shell will print "exit 139" for this and accept the next command.
 *
 * If isolation were broken, the store would either succeed (and we'd print the FAILED line) or
 * the kernel itself would fault — both regressions this test catches.
 */
#include <unistd.h>

int main(void)
{
	write(1, "[segv] write kernel SRAM\n", 25);
	*(volatile unsigned int *)0x20000000u = 0xdeadbeefu;
	write(1, "[segv] isolation FAILED\n", 24); /* unreachable if contained */
	return 0;
}
