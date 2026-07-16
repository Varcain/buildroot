/* Hard-float nested-signal regression for the oveRTOS Linux personality.
 * SIGUSR1 raises SIGUSR2 while its handler is active.  Patterns in all 32 VFP
 * registers verify that both guest contexts return in LIFO order. */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static uint32_t main_pattern[32] __attribute__((aligned(8)));
static uint32_t outer_pattern[32] __attribute__((aligned(8)));
static uint32_t inner_pattern[32] __attribute__((aligned(8)));
static uint32_t main_observed[32] __attribute__((aligned(8)));
static uint32_t outer_observed[32] __attribute__((aligned(8)));
static volatile sig_atomic_t outer_seen;
static volatile sig_atomic_t inner_seen;

static void fill(uint32_t *dst, uint32_t seed)
{
	for (unsigned i = 0; i < 32; ++i)
		dst[i] = seed ^ (i * 0x01010101u);
}

/* Deliberately violate the normal callee-saved VFP convention inside the inner
 * signal handler.  The kernel's sigreturn, not a compiler-generated epilogue,
 * must restore the interrupted outer handler's complete FP state. */
static __attribute__((naked, noinline)) void
load_all(const uint32_t *src __attribute__((unused)))
{
	__asm__ volatile("vldmia r0, {s0-s31}\n"
			 "bx lr");
}

static __attribute__((noinline)) void signal_with_fp(pid_t pid, int sig,
					      const uint32_t *src, uint32_t *dst)
{
	register long r0 __asm__("r0") = pid;
	register long r1 __asm__("r1") = sig;
	register long r7 __asm__("r7") = 37; /* ARM EABI kill(2) */
	__asm__ volatile("vldmia %2, {s0-s31}\n"
			 "svc 0\n"
			 "vstmia %3, {s0-s31}\n"
			 : "+r"(r0), "+r"(r1)
			 : "r"(src), "r"(dst), "r"(r7)
			 : "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
			   "s8", "s9", "s10", "s11", "s12", "s13", "s14", "s15",
			   "s16", "s17", "s18", "s19", "s20", "s21", "s22", "s23",
			   "s24", "s25", "s26", "s27", "s28", "s29", "s30", "s31",
			   "cc", "memory");
}

static void inner_handler(int sig)
{
	(void)sig;
	inner_seen = 1;
	load_all(inner_pattern);
}

static void outer_handler(int sig)
{
	(void)sig;
	outer_seen = 1;
	signal_with_fp(getpid(), SIGUSR2, outer_pattern, outer_observed);
}

int main(void)
{
	struct sigaction sa;
	puts("nested-signal-hardfloat-start");
	fill(main_pattern, 0x3f000000u);
	fill(outer_pattern, 0x41000000u);
	fill(inner_pattern, 0x42000000u);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = inner_handler;
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGUSR2, &sa, NULL) != 0) {
		perror("sigaction SIGUSR2");
		return 2;
	}
	sa.sa_handler = outer_handler;
	if (sigaction(SIGUSR1, &sa, NULL) != 0) {
		perror("sigaction SIGUSR1");
		return 2;
	}

	signal_with_fp(getpid(), SIGUSR1, main_pattern, main_observed);
	if (!outer_seen || !inner_seen) {
		printf("nested-signal-FAIL delivery outer=%d inner=%d\n",
		       outer_seen, inner_seen);
		return 1;
	}
	if (memcmp(outer_pattern, outer_observed, sizeof(outer_pattern)) != 0) {
		puts("nested-signal-FAIL outer FP restore");
		return 1;
	}
	if (memcmp(main_pattern, main_observed, sizeof(main_pattern)) != 0) {
		puts("nested-signal-FAIL main FP restore");
		return 1;
	}
	puts("nested-signal-hardfloat-ok");
	return 0;
}
