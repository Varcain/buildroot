/* kstress — kernel-hardening attacker test for the oveRTOS Linux personality.
 *
 * An UNPRIVILEGED Linux program that tries to crash / corrupt / leak the KERNEL through paths the MPU
 * isolation does NOT cover — chiefly the syscall boundary: a bad pointer the PRIVILEGED syscall handler
 * dereferences on the program's behalf (a confused deputy — the MPU can't stop the kernel's own
 * access), a string with no terminator, a non-MemManage CPU fault, and resource exhaustion. After the
 * hardening every probe must be REJECTED (-EFAULT/-ENOEXEC/-ENOMEM) or CONTAINED (exit 139), with the
 * shell surviving. Each mode exits 0 on PASS so the driver can grade it via `echo $?`.
 *
 * Usage: kstress <ptr|str|udf|div0|rsrc|elf>
 *   ptr  - getcwd/write/openat each fed a kernel / device / just-past / just-before-region pointer;
 *          access_ok must reject all with -EFAULT (never let the privileged handler deref them).
 *   str  - openat a path with no NUL before the region end; user_strnlen must stop at the region
 *          boundary and return -EFAULT, not walk a strlen off into kernel memory.
 *   udf  - an undefined instruction (UsageFault) must be CONTAINED (exit 139), not HardFault/panic.
 *   div0 - integer divide-by-zero: contained (139) if DIV_0_TRP is on, else benign — either is fine.
 *   rsrc - exhaust the fd table (open in a loop); the limit must return -EMFILE, never a wild fd/OOB.
 *   elf  - execve a non-ELF file; the loader must reject it (kernel survives), never parse OOB/crash.
 */
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

extern char **environ;

/* Program MPU region size (matches OVE_LNX_PROG_REGION_SIZE); a region is aligned to its size. */
#define REGION 0x80000u

static void W(const char *s)
{
	write(1, s, strlen(s));
}

static void Wn(unsigned n)
{
	char t[12], b[12];
	int k = 0, i = 0;
	if (!n) {
		write(1, "0", 1);
		return;
	}
	while (n) {
		t[k++] = (char)('0' + n % 10);
		n /= 10;
	}
	while (k)
		b[i++] = t[--k];
	write(1, b, (size_t)i);
}

/* Feed one bad pointer to each user-pointer direction; return how many were rejected (of 3):
 *   getcwd - kernel WRITES p   (access_ok write; getcwd, not read, so it never blocks on console)
 *   write  - kernel READS p    (access_ok read)
 *   openat - kernel walks a string at p (user_strnlen)
 * All three must return -EFAULT so the privileged handler never dereferences p. */
static int probe_all(uintptr_t p)
{
	int ok = 0;
	errno = 0;
	if (getcwd((char *)p, 64) == NULL && errno == EFAULT)
		ok++;
	errno = 0;
	if (write(1, (const void *)p, 64) < 0 && errno == EFAULT)
		ok++;
	errno = 0;
	if (openat(AT_FDCWD, (const char *)p, O_RDONLY) < 0 && errno == EFAULT)
		ok++;
	return ok;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "ptr";

	if (!strcmp(mode, "ptr")) {
		/* Derive the just-outside pointers from our own region (base is REGION-aligned) rather
		 * than hard-coding an address — a fixed "unmapped" constant can land inside another
		 * engine's program region and make the kernel-writes-p probe block instead of fault. */
		char anchor;
		uintptr_t base = (uintptr_t)&anchor & ~(uintptr_t)(REGION - 1);
		uintptr_t bad[4] = {0x20000000u, 0xE000ED00u, base + REGION, base - 16};
		const char *nm[4] = {"kernel-SRAM", "device", "just-past-region", "just-before-region"};
		int ok = 0, n = 0;

		W("[kstress] ptr: feeding bad pointers to getcwd/write/openat\n");
		for (int i = 0; i < 4; i++) {
			int r = probe_all(bad[i]);
			ok += r;
			n += 3;
			W("[kstress]   ");
			W(nm[i]);
			W(": ");
			Wn((unsigned)r);
			W("/3 rejected\n");
		}
		W("[kstress] ptr: ");
		Wn((unsigned)ok);
		W("/");
		Wn((unsigned)n);
		W(" rejected\n");
		W(ok == n ? "[kstress] ptr: PASS\n" : "[kstress] ptr: FAIL\n");
		return ok == n ? 0 : 1;
	}

	if (!strcmp(mode, "str")) {
		char anchor;
		uintptr_t base = (uintptr_t)&anchor & ~(uintptr_t)(REGION - 1);
		volatile char *last = (volatile char *)(base + REGION - 1); /* last byte of our region */

		W("[kstress] str: unterminated path must not walk off the region end\n");
		*last = 'A'; /* in-region write: a non-NUL byte with only the region edge after it */
		errno = 0;
		int r = openat(AT_FDCWD, (const char *)last, O_RDONLY);
		int ok = (r < 0 && errno == EFAULT); /* strnlen hit region_hi with no NUL */
		W(ok ? "[kstress] str: PASS\n" : "[kstress] str: FAIL\n");
		return ok ? 0 : 1;
	}

	if (!strcmp(mode, "udf")) {
		W("[kstress] udf: executing an undefined instruction\n");
		__asm__ volatile("udf #0"); /* UsageFault -> must be contained (139), not a HardFault */
		W("[kstress] udf: NOT CONTAINED\n"); /* unreachable if contained */
		return 1;
	}

	if (!strcmp(mode, "div0")) {
		volatile int z = 0;
		volatile int q;

		W("[kstress] div0: integer divide by zero\n");
		q = 1 / z; /* DIV_0_TRP on -> UsageFault -> 139; off -> benign (returns 0) */
		(void)q;
		W("[kstress] div0: survived (not trapped, benign)\n"); /* skipped if contained */
		return 0;
	}

	if (!strcmp(mode, "rsrc")) {
		int fds[256], nf = 0, hit = 0;

		W("[kstress] rsrc: exhausting the fd table must return EMFILE, not OOB or crash\n");
		for (int i = 0; i < 256; i++) {
			int fd = open("/etc/passwd", O_RDONLY);
			if (fd < 0) { /* the table filled — a clean errno, never a wild fd or fault */
				hit = (errno == EMFILE || errno == ENFILE);
				break;
			}
			fds[nf++] = fd;
		}
		for (int i = 0; i < nf; i++) /* release them so the shell keeps its own fds */
			close(fds[i]);
		W("[kstress]   opened ");
		Wn((unsigned)nf);
		W(" before the limit\n");
		W(hit ? "[kstress] rsrc: PASS\n" : "[kstress] rsrc: FAIL (no clean limit)\n");
		return hit ? 0 : 1;
	}

	if (!strcmp(mode, "elf")) {
		char *av[2] = {"/etc/passwd", 0};

		W("[kstress] elf: execve a non-ELF file must be rejected, not crash the kernel\n");
		errno = 0;
		execve("/etc/passwd", av, environ); /* a text file: no ELF magic, no #! */
		/* If execve RETURNS it rejected the load cleanly (-ENOEXEC → PASS here). On a personality
		 * that tears the caller down instead, we never reach this and the driver just sees a
		 * non-zero $? for `kstress elf` — either way the loader refused it and the kernel lives. */
		int ok = (errno == ENOEXEC);
		W(ok ? "[kstress] elf: PASS\n" : "[kstress] elf: FAIL\n");
		return ok ? 0 : 1;
	}

	W("[kstress] unknown mode\n");
	return 2;
}
