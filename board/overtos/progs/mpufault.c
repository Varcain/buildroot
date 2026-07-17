/* Negative MPU-isolation tests for the fault classes segv.c / xregion.c / kstress.c
 * leave uncovered. Each mode makes ONE unprivileged access the MPU must deny, and
 * the personality must contain it: the program dies like a default-action SIGSEGV
 * (exit 139), the coordinator reaps it, the kernel and the shell are untouched.
 * A mode that returns instead prints its FAILED line — that is the regression.
 *
 * Usage: mpufault <kread|periph|nxexec>
 *   kread  - READ kernel SRAM. segv.c only ever writes it, so nothing proved the
 *            region is unreadable rather than merely unwritable; a read-allowed
 *            region leaks kernel state to any guest that asks.
 *   periph - READ a peripheral directly (SCB CPUID, 0xE000ED00). kstress's `ptr`
 *            mode hands that address to *syscalls*, which the kernel's own
 *            validator rejects; this instead takes the access from unprivileged
 *            code, so it tests the MPU/PPB rather than the syscall boundary.
 *   nxexec - EXECUTE from a writable data region. The personality maps guest data
 *            EXECUTE_NEVER (portMPU_REGION_EXECUTE_NEVER), so branching into a
 *            buffer must raise MemManage on the instruction fetch. Without XN a
 *            guest bug becomes arbitrary code execution inside its own region.
 *
 * Not covered here, deliberately: divide-by-zero and unaligned access. Both need
 * SCB->CCR traps (DIV_0_TRP bit 4, UNALIGN_TRP bit 3) that this target does not
 * enable — only bit 18 (branch prediction) is set. Unaligned trapping would fault
 * ordinary uClibc/BusyBox code, and DIV_0_TRP is a global switch that would also
 * make host divide-by-zero fatal, so both stay off and neither fault class exists
 * to test.
 */
#include <string.h>
#include <unistd.h>

static void W(const char *s)
{
	write(1, s, strlen(s));
}

/* A writable buffer holding one `bx lr` (Thumb). Executing it must fault on the
 * fetch, not on any access it makes — so the body itself is harmless. */
static volatile unsigned short nx_code[] = {0x4770u, 0x4770u};

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "kread";

	if (!strcmp(mode, "kread")) {
		W("[mpufault] read kernel SRAM\n");
		unsigned int v = *(volatile unsigned int *)0x20000000u;
		/* Consume the value so the load cannot be optimised away, and so a
		 * broken deny is visible rather than silently discarded. */
		if (v == 0xffffffffu)
			W("[mpufault] kread: read 0xffffffff\n");
		W("[mpufault] kread: isolation FAILED\n");
		return 1;
	}

	if (!strcmp(mode, "periph")) {
		W("[mpufault] read SCB CPUID (0xE000ED00)\n");
		unsigned int v = *(volatile unsigned int *)0xE000ED00u;
		if (v == 0xffffffffu)
			W("[mpufault] periph: read 0xffffffff\n");
		W("[mpufault] periph: isolation FAILED\n");
		return 1;
	}

	if (!strcmp(mode, "nxexec")) {
		W("[mpufault] execute from data region\n");
		/* Branch straight at the buffer, in assembly. A plain
		 *     void (*fn)(void) = (void (*)(void))nx_code; fn();
		 * does NOT do this on FDPIC: a function pointer there is a
		 * {entry, GOT} descriptor, so the call dereferences the buffer AS a
		 * descriptor and branches to whatever its first word happens to
		 * contain. That faults too — with the same IACCVIOL — so it looks
		 * like a passing test while never executing the buffer at all.
		 * Bit 0 keeps the core in Thumb state; nx_code holds `bx lr`, so if
		 * the fetch were ever permitted it would simply return and the FAILED
		 * line below would print. */
		unsigned long target = ((unsigned long)nx_code) | 1UL;
		__asm__ volatile("blx %0" : : "r"(target) : "lr", "memory");
		W("[mpufault] nxexec: isolation FAILED\n");
		return 1;
	}

	W("[mpufault] usage: mpufault <kread|periph|nxexec>\n");
	return 2;
}
