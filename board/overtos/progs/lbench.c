/*
 * lbench.c — oveRTOS Linux-personality microbenchmark.
 *
 * Measures the "personality tax": the cost of running work as an unprivileged
 * FDPIC userspace process (SVC syscall boundary + MPU + run-loop coordinator)
 * versus the SAME work as a native privileged ove_thread.  The native side is
 * tests/benchmarks/c in the oveRTOS tree; this program is its userspace mirror.
 *
 * Timing: clock_gettime(CLOCK_MONOTONIC), which the personality backs with
 * ove_time_get_ns — the DWT cycle counter (DWT->CYCCNT) on FreeRTOS, exactly
 * the register the native harness's bench_cyccnt reads, so on real silicon both
 * sides are cycle-accurate off one clock.  Each case brackets INNER ops between
 * two clock reads and divides; WINDOWS repeats give a distribution (min = true
 * cost, filtering coordinator/SysTick preemption; plus p50/p95/p99/mean).  All
 * figures are nanoseconds — same unit as the native harness → comparable.
 *
 * Output: the same ###BENCH_JSON_BEGIN/###BENCH_JSON_END envelope the native
 * harness emits (tests/benchmarks/c/src/bench_output.c).  rtos is left "linux";
 * the host driver knows which engine it flashed and does the per-axis pairing.
 *
 * Axes:  B1 compute_mix | B2 null_syscall/getpid_cached/clock_gettime |
 *        B3 write_devnull | B5 ctx_switch (pipe ping-pong w/ a co-process) |
 *        B6 pipe_wr_4k (throughput to a draining child) | B7 spawn_vfork_exec.
 * The multi-process axes re-exec this same binary in a child mode (pong/sink/
 * nop); pipe fd numbers are passed via argv (the child closes the rest AFTER
 * exec — its own fd table — so the vfork window never touches the shared one).
 *
 * KEEP bench_kernel_mix + BENCH_KERNEL_CHECKSUM in sync with
 * tests/benchmarks/c/include/bench_kernel.h (self-checked at startup).
 *
 * Build: arm-buildroot-uclinuxfdpiceabi-gcc -mfdpic -O2 lbench.c -o lbench
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

/* ---- shared compute kernel (KEEP IN SYNC with bench_kernel.h) ------------- */
#define BENCH_KERNEL_CHECKSUM 0x855ee3aau /* bench_kernel_mix(1) */

static uint32_t bench_kernel_mix(uint32_t seed)
{
	uint32_t x = seed ? seed : 0x2545F491u;
	uint32_t h = 2166136261u; /* FNV-1a offset basis */
	for (int i = 0; i < 256; i++) {
		x ^= x << 13; /* xorshift32 */
		x ^= x >> 17;
		x ^= x << 5;
		h = (h ^ x) * 16777619u; /* FNV-1a prime */
	}
	return h;
}

/* ---- timing -------------------------------------------------------------- */
static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- windowed harness ---------------------------------------------------- */
#define MAX_WINDOWS 400u

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return (x > y) - (x < y);
}

static int g_first_case = 1;

static void emit_case(const char *name, const char *type, uint64_t *s, unsigned int w)
{
	if (!w) {
		printf("%s{\"name\":\"%s\",\"type\":\"%s\",\"min_ns\":0,\"max_ns\":0,\"avg_ns\":0,"
		       "\"count\":0,\"ops_per_sec\":0,\"p50_ns\":0,\"p95_ns\":0,\"p99_ns\":0,"
		       "\"trimmed_mean_ns\":0,\"stddev_ns_q1000\":0}",
		       g_first_case ? "" : ",", name, type);
		g_first_case = 0;
		return;
	}
	qsort(s, w, sizeof(uint64_t), cmp_u64);
	uint64_t mn = s[0], mx = s[w - 1];
	uint64_t p50 = s[w / 2], p95 = s[(w * 95) / 100], p99 = s[(w * 99) / 100];
	uint64_t sum = 0;
	for (unsigned int i = 0; i < w; i++)
		sum += s[i];
	uint64_t avg = sum / w;
	unsigned int keep = w - w / 100; /* trimmed mean: drop top 1% */
	uint64_t tsum = 0;
	for (unsigned int i = 0; i < keep; i++)
		tsum += s[i];
	uint64_t tmean = keep ? tsum / keep : avg;
	unsigned int ops = avg ? (unsigned int)(1000000000ull / avg) : 0;

	printf("%s{\"name\":\"%s\",\"type\":\"%s\","
	       "\"min_ns\":%u,\"max_ns\":%u,\"avg_ns\":%u,\"count\":%u,\"ops_per_sec\":%u,"
	       "\"p50_ns\":%u,\"p95_ns\":%u,\"p99_ns\":%u,\"trimmed_mean_ns\":%u,\"stddev_ns_q1000\":0}",
	       g_first_case ? "" : ",", name, type, (unsigned int)mn, (unsigned int)mx,
	       (unsigned int)avg, w, ops, (unsigned int)p50, (unsigned int)p95, (unsigned int)p99,
	       (unsigned int)tmean);
	g_first_case = 0;
}

/* Run `body` in WINDOWS windows of `inner` ops; per-op ns = elapsed/inner.
 * Windows where the clock ran backwards (the 32-bit DWT wraps ~every 20 s) are
 * discarded and retried, bounded, so one wrap can't pollute max/avg. */
static uint64_t g_s[MAX_WINDOWS];

static void bench(const char *name, const char *type, void (*body)(void *), void *ctx,
		  unsigned int inner, unsigned int windows)
{
	if (windows > MAX_WINDOWS)
		windows = MAX_WINDOWS;
	if (!inner)
		inner = 1;
	for (unsigned int i = 0; i < 8; i++) /* warmup */
		body(ctx);
	unsigned int got = 0, att = 0, cap = windows * 8u + 64u;
	while (got < windows && att < cap) {
		att++;
		uint64_t t0 = now_ns();
		for (unsigned int i = 0; i < inner; i++)
			body(ctx);
		uint64_t t1 = now_ns();
		if (t1 > t0)
			g_s[got++] = (t1 - t0) / inner;
	}
	emit_case(name, type, g_s, got);
}

/* ---- simple-op case bodies (B1/B2/B3) ------------------------------------ */
static volatile uint32_t g_sink; /* defeat dead-code elimination */

static void body_compute_mix(void *c)
{
	(void)c;
	g_sink ^= bench_kernel_mix(g_sink ? g_sink : 1u);
}

/* Raw syscall() bypasses uClibc's getpid cache so every call really traps. */
static void body_null_syscall(void *c)
{
	(void)c;
	g_sink += (uint32_t)syscall(SYS_getpid);
}

static void body_getpid_cached(void *c)
{
	(void)c;
	g_sink += (uint32_t)getpid();
}

static void body_clock_gettime(void *c)
{
	(void)c;
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	g_sink += (uint32_t)ts.tv_nsec;
}

static int g_devnull = -1;
static char g_wbuf[64];
static void body_write_devnull(void *c)
{
	(void)c;
	g_sink += (uint32_t)write(g_devnull, g_wbuf, sizeof(g_wbuf));
}

/* ---- child modes (co-process for the multi-process axes) ----------------- */
static void close_all_but(int a, int b)
{
	for (int fd = 3; fd < 32; fd++)
		if (fd != a && fd != b)
			close(fd);
}

/* B5 peer: echo one byte back forever (EOF on the read end => exit). */
static int child_pong(int rfd, int wfd)
{
	close_all_but(rfd, wfd);
	char ch;
	for (;;) {
		if (read(rfd, &ch, 1) <= 0)
			break;
		if (write(wfd, &ch, 1) != 1)
			break;
	}
	return 0;
}

/* B6 peer: drain the pipe as fast as possible until EOF. */
static int child_sink(int rfd)
{
	close_all_but(rfd, rfd);
	static char b[4096];
	while (read(rfd, b, sizeof(b)) > 0)
		;
	return 0;
}

static pid_t spawn(char *const av[])
{
	pid_t pid = vfork();
	if (pid == 0) {
		execve("/usr/bin/lbench", av, environ);
		_exit(127);
	}
	return pid;
}

/* ---- multi-process axes (B5/B6/B7) --------------------------------------- */

/* B5 — one round-trip = 2 process switches through the run-loop coordinator. */
static void bench_ctx_switch(unsigned int windows)
{
	int p2c[2], c2p[2];
	if (pipe(p2c) || pipe(c2p))
		return;
	char rfd[8], wfd[8];
	snprintf(rfd, sizeof(rfd), "%d", p2c[0]);
	snprintf(wfd, sizeof(wfd), "%d", c2p[1]);
	char *av[] = {"lbench", "pong", rfd, wfd, NULL};
	pid_t pid = spawn(av);
	close(p2c[0]);
	close(c2p[1]); /* parent doesn't use the child's ends */
	int W = p2c[1], R = c2p[0];
	char ch = 'x';
	for (int i = 0; i < 8; i++) { /* warmup */
		if (write(W, &ch, 1) != 1 || read(R, &ch, 1) != 1)
			break;
	}
	unsigned int got = 0, att = 0, cap = windows * 8u + 64u;
	while (got < windows && att < cap) {
		att++;
		uint64_t t0 = now_ns();
		if (write(W, &ch, 1) != 1 || read(R, &ch, 1) != 1)
			break;
		uint64_t t1 = now_ns();
		if (t1 > t0)
			g_s[got++] = t1 - t0;
	}
	close(W); /* child sees EOF -> exits */
	int st;
	waitpid(pid, &st, 0);
	close(R);
	emit_case("ctx_switch", "latency", g_s, got);
}

/* B6 — write 4 KiB chunks to a draining child; the small pipe ring back-
 * pressures, so this is the personality's realistic pipe data-path rate. */
static void bench_pipe_tput(unsigned int windows)
{
	int p[2];
	if (pipe(p))
		return;
	char rfd[8];
	snprintf(rfd, sizeof(rfd), "%d", p[0]);
	char *av[] = {"lbench", "sink", rfd, NULL};
	pid_t pid = spawn(av);
	close(p[0]);
	int W = p[1];
	static char buf[4096];
	memset(buf, 0xa5, sizeof(buf));
	for (int i = 0; i < 4; i++) /* warmup */
		if (write(W, buf, sizeof(buf)) != (ssize_t)sizeof(buf))
			break;
	unsigned int got = 0, att = 0, cap = windows * 8u + 64u;
	while (got < windows && att < cap) {
		att++;
		uint64_t t0 = now_ns();
		size_t sent = 0; /* the ring is < 4 KiB: loop over partial writes */
		while (sent < sizeof(buf)) {
			ssize_t n = write(W, buf + sent, sizeof(buf) - sent);
			if (n <= 0)
				break;
			sent += (size_t)n;
		}
		uint64_t t1 = now_ns();
		if (sent == sizeof(buf) && t1 > t0)
			g_s[got++] = t1 - t0; /* ns per 4 KiB (drain-limited) */
	}
	close(W);
	int st;
	waitpid(pid, &st, 0);
	emit_case("pipe_wr_4k", "throughput", g_s, got);
}

/* B7 — spawn a unit of execution: vfork + execve(this binary, nop) + wait.
 * The personality's most expensive path (FDPIC load + MPU region setup). Kept
 * to few windows: NOMMU vfork+exec is delicate and each spawn is ms-scale. */
static void bench_spawn(unsigned int windows)
{
	char *av[] = {"lbench", "nop", NULL};
	for (int i = 0; i < 3; i++) { /* warmup */
		int st;
		waitpid(spawn(av), &st, 0);
	}
	unsigned int got = 0, att = 0, cap = windows * 4u + 16u;
	while (got < windows && att < cap) {
		att++;
		uint64_t t0 = now_ns();
		pid_t pid = spawn(av);
		int st;
		waitpid(pid, &st, 0);
		uint64_t t1 = now_ns();
		if (pid > 0 && t1 > t0)
			g_s[got++] = t1 - t0;
	}
	emit_case("spawn_vfork_exec", "latency", g_s, got);
}

/* ---- main ---------------------------------------------------------------- */
int main(int argc, char **argv)
{
	if (argc > 1) { /* child modes (re-exec of this binary) */
		if (!strcmp(argv[1], "nop"))
			return 0;
		if (!strcmp(argv[1], "pong") && argc >= 4)
			return child_pong(atoi(argv[2]), atoi(argv[3]));
		if (!strcmp(argv[1], "sink") && argc >= 3)
			return child_sink(atoi(argv[2]));
		return 2; /* unknown mode */
	}

	uint32_t ck = bench_kernel_mix(1u);
	g_devnull = open("/dev/null", O_WRONLY);

	printf("###BENCH_JSON_BEGIN\n");
	printf("{\"rtos\":\"linux\",\"binding\":\"linux\",\"suite\":\"personality\","
	       "\"kernel_checksum\":\"0x%08x\",\"kernel_ok\":%d,\"cases\":[",
	       ck, ck == BENCH_KERNEL_CHECKSUM ? 1 : 0);

	bench("compute_mix", "latency", body_compute_mix, 0, 64, 256);
	bench("null_syscall", "latency", body_null_syscall, 0, 1000, 200);
	bench("getpid_cached", "latency", body_getpid_cached, 0, 1000, 200);
	bench("clock_gettime", "latency", body_clock_gettime, 0, 500, 200);
	if (g_devnull >= 0)
		bench("write_devnull", "latency", body_write_devnull, 0, 500, 200);

	bench_ctx_switch(300);
	bench_pipe_tput(200);
	bench_spawn(40);

	printf("]}\n");
	printf("###BENCH_JSON_END\n");

	if (g_devnull >= 0)
		close(g_devnull);
	return ck == BENCH_KERNEL_CHECKSUM ? 0 : 1;
}
