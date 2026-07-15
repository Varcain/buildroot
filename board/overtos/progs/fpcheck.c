/* Hard-float FDPIC runtime regression. Verify the complete single-precision VFP
 * context across both the normal deferred bottom half and a blocking syscall. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static __attribute__((noinline)) float hard_add(float a, float b)
{
	return a + b;
}

static int check_context(long nr, long a0_in, long a1_in, long a2_in, uint32_t seed,
			 long expected_ret)
{
	uint32_t saved_fpscr;
	__asm__ volatile("vmrs %0, fpscr" : "=r"(saved_fpscr));

	uint32_t expected[32] __attribute__((aligned(8)));
	uint32_t observed[32] __attribute__((aligned(8)));
	for (unsigned i = 0; i < 32; i++) {
		expected[i] = 0x3f000000u ^ seed ^ (i * 0x01010101u);
		observed[i] = 0;
	}

	register long a0 __asm__("r0") = a0_in;
	register long a1 __asm__("r1") = a1_in;
	register long a2 __asm__("r2") = a2_in;
	register const uint32_t *src __asm__("r4") = expected;
	register uint32_t *dst __asm__("r5") = observed;
	register uint32_t fpscr __asm__("r6") = 0x00400000u;
	register long syscall_nr __asm__("r7") = nr;

	/* No compiler-generated VFP instruction can run between the resume point
	 * and snapshot because the entire load/SVC/store sequence is one asm block. */
	__asm__ volatile("vldmia r4, {s0-s31}\n"
			 "vmsr  fpscr, r6\n"
			 "svc   0\n"
			 "vstmia r5, {s0-s31}\n"
			 "vmrs  r6, fpscr\n"
			 : "+r"(a0), "+r"(a1), "+r"(a2), "+r"(src), "+r"(dst),
			   "+r"(fpscr), "+r"(syscall_nr)
			 :
			 : "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
			   "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15",
			   "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
			   "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31",
			   "cc", "memory");
	__asm__ volatile("vmsr fpscr, %0" : : "r"(saved_fpscr) : "memory");

	if (a0 != expected_ret || fpscr != 0x00400000u)
		return 0;
	for (unsigned i = 0; i < 32; i++)
		if (observed[i] != expected[i])
			return 0;
	return 1;
}

int main(int argc, char **argv)
{
	unsigned loops = 100;
	if (argc > 1) {
		unsigned long parsed = strtoul(argv[1], NULL, 0);
		if (parsed > 0 && parsed <= 100000)
			loops = (unsigned)parsed;
	}

	volatile float a = 1.25f, b = 2.5f;
	if (hard_add(a, b) != 3.75f) {
		puts("hardfloat-fp-FAIL: ABI arithmetic");
		return 1;
	}
	for (unsigned i = 0; i < loops; i++) {
		uint32_t seed = 0x9e3779b9u * (i + 1u);
		if (!check_context(999, 0, 0, 0, seed, -38) ||
		    !check_context(168 /* poll */, 0, 0, 1, ~seed, 0)) {
			printf("hardfloat-fp-context-fail iteration=%u\n", i);
			return 1;
		}
	}
	printf("hardfloat-fp-ok loops=%u\n", loops);
	return 0;
}
