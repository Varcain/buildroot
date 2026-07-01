/* Negative INTER-PROGRAM MPU-isolation test for the Linux personality (Phase 2).
 *
 * Where segv.c writes KERNEL SRAM (0x20000000) — denied by the ARM default map, proving KERNEL
 * isolation — this writes a SIBLING program's pool region, denied by the privileged-only whole-pool
 * BASE MPU region (unpriv-NO). That base-region deny is exactly the Phase-2 inter-program
 * enforcement path, which segv.c never exercises.
 *
 * The program's stack sits inside its own 512K-aligned pool region (OVE_LNX_PROG_REGION_SIZE); the
 * next 512K region up — sp rounded down to 512K, plus 512K — is another program's region (or a
 * dyn_pool / unmapped pool address), all ungranted to this program. The unprivileged store there
 * raises MemManage; the personality contains it (exit 139) and the shell survives. Board-
 * independent — no pool base is hardcoded (the sibling address is derived from the running sp).
 *
 * If inter-program isolation were broken the store would return (we'd print the FAILED line) or the
 * kernel itself would fault — both regressions this test catches.
 */
#include <unistd.h>

int main(void)
{
	unsigned long sp;
	__asm__ volatile("mov %0, sp" : "=r"(sp));
	/* 0x80000 == 512K == OVE_LNX_PROG_REGION_SIZE; the per-program MPU region is 512K-aligned. */
	unsigned long region_base = sp & ~0x7FFFFUL;
	volatile unsigned int *sibling = (volatile unsigned int *)(region_base + 0x80000UL);

	write(1, "[xregion] write sibling pool region\n", 36);
	*sibling = 0xdeadbeefu;
	write(1, "[xregion] isolation FAILED\n", 27); /* unreachable if contained */
	return 0;
}
