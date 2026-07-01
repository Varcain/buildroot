/* kstress — kernel-hardening attacker test for the oveRTOS Linux personality.
 *
 * An UNPRIVILEGED Linux program that tries to crash / corrupt / leak the KERNEL through paths the MPU
 * isolation does NOT cover — chiefly the syscall boundary: a bad pointer that the PRIVILEGED syscall
 * handler dereferences on the program's behalf (a confused deputy — the MPU can't stop the kernel's
 * own access), and a non-MemManage CPU fault. After the hardening every probe must be REJECTED
 * (-EFAULT) or CONTAINED (exit 139), with the shell surviving. BEFORE it, the `ptr` mode crashes or
 * leaks the kernel — that failing run is the differential baseline.
 *
 * Usage: kstress <ptr|udf>
 */
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>

static void W(const char *s)
{
	write(1, s, strlen(s));
}

/* write(fd, p, n): the kernel READS n bytes from p and copies them to the console. A bad p must be
 * rejected (-EFAULT), never dereferenced — else it faults (crash) or leaks kernel bytes out. */
static int probe_wr(const char *name, uintptr_t p)
{
	errno = 0;
	long r = write(1, (const void *)p, 8); /* if unmapped -> privileged fault -> we never return */
	W("\n[kstress]   ");
	W(name);
	if (r < 0 && errno == EFAULT) {
		W(": EFAULT (rejected)\n");
		return 1;
	}
	W(": NOT REJECTED (kernel reached)\n");
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "ptr";

	if (!strcmp(mode, "ptr")) {
		W("[kstress] ptr: feeding bad pointers to write()\n");
		int ok = 0, n = 0;
		n++;
		ok += probe_wr("unmapped 0x90000000", 0x90000000u);
		n++;
		ok += probe_wr("kernel-SRAM 0x20000000", 0x20000000u);
		n++;
		ok += probe_wr("device 0xE000ED00", 0xE000ED00u);
		W(ok == n ? "[kstress] ptr: PASS\n" : "[kstress] ptr: FAIL\n");
		return ok == n ? 0 : 1;
	}

	if (!strcmp(mode, "udf")) {
		W("[kstress] udf: executing an undefined instruction\n");
		__asm__ volatile("udf #0"); /* UsageFault -> must be contained (139), not a HardFault/panic */
		W("[kstress] udf: NOT CONTAINED\n"); /* unreachable if contained */
		return 1;
	}

	W("[kstress] unknown mode\n");
	return 2;
}
