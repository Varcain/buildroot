/* Fault-containment regression for Cortex-M lazy floating-point stacking.
 *
 * Activate the VFP context, replace PSP with a read-only QSPI address, then
 * make an MPU-prohibited kernel-SRAM write.  Exception entry cannot create the
 * core/FP frame at that PSP, so the host must abandon it, synthesize a frame on
 * the task's internal trampoline stack, and reap this process.  The invoking
 * shell must remain usable and report exit status 139.
 */
#include <unistd.h>

#if defined(__ARM_FP) && (__ARM_FP != 0)
__attribute__((naked, noreturn)) static void trigger_bad_fp_stack(void)
{
	__asm__ volatile("vmov s0, r0\n"
			 "ldr  r0, =0x90083158\n"
			 "mov  sp, r0\n"
			 "ldr  r1, =0x20000000\n"
			 "str  r0, [r1]\n"
			 "b    .\n");
}
#endif

int main(void)
{
#if defined(__ARM_FP) && (__ARM_FP != 0)
	static const char msg[] = "[fpbadsp] FP-active invalid PSP\n";
	(void)write(1, msg, sizeof(msg) - 1u);
	trigger_bad_fp_stack();
#else
	static const char msg[] = "[fpbadsp] requires a hard-float guest\n";
	(void)write(1, msg, sizeof(msg) - 1u);
	return 77;
#endif
}
