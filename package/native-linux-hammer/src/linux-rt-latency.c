// SPDX-License-Identifier: MIT
/* Absolute-clock timer-to-SCHED_FIFO latency probe for the Linux baseline. */

#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PERIOD_NS 1000000ULL
#define WORK_ITERATIONS 512U
#define HISTOGRAM_BINS 18U

static const uint32_t histogram_upper_us[HISTOGRAM_BINS] = {
	1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20, 32, 50, 100, 250, 500, 750, 1000,
};
static volatile uint32_t payload_state = 0x6d2b79f5U;

static uint64_t timespec_ns(const struct timespec *value)
{
	return (uint64_t)value->tv_sec * 1000000000ULL +
	       (uint64_t)value->tv_nsec;
}

static struct timespec ns_timespec(uint64_t value)
{
	struct timespec result = {
		.tv_sec = (time_t)(value / 1000000000ULL),
		.tv_nsec = (long)(value % 1000000000ULL),
	};

	return result;
}

static unsigned int histogram_bin(uint64_t nanoseconds)
{
	unsigned int index;

	for (index = 0; index < HISTOGRAM_BINS; ++index) {
		if (nanoseconds <= (uint64_t)histogram_upper_us[index] * 1000ULL)
			return index;
	}
	return HISTOGRAM_BINS - 1U;
}

static uint32_t percentile_upper(const uint64_t *histogram, uint64_t samples,
				 unsigned int per_mille)
{
	uint64_t target = (samples * per_mille + 999U) / 1000U;
	uint64_t cumulative = 0;
	unsigned int index;

	for (index = 0; index < HISTOGRAM_BINS; ++index) {
		cumulative += histogram[index];
		if (cumulative >= target)
			return histogram_upper_us[index];
	}
	return histogram_upper_us[HISTOGRAM_BINS - 1U];
}

static int parse_duration(const char *text, unsigned int *duration)
{
	char *end;
	unsigned long value;

	errno = 0;
	value = strtoul(text, &end, 10);
	if (errno != 0 || *text == '\0' || *end != '\0' || value == 0 ||
	    value > 3600)
		return -1;
	*duration = (unsigned int)value;
	return 0;
}

int main(int argc, char **argv)
{
	uint64_t dispatch_histogram[HISTOGRAM_BINS] = { 0 };
	uint64_t planned_releases;
	uint64_t releases = 0;
	uint64_t executions = 0;
	uint64_t missed = 0;
	uint64_t late_finishes = 0;
	uint64_t dispatch_min = UINT64_MAX;
	uint64_t dispatch_max = 0;
	uint64_t dispatch_sum = 0;
	uint64_t work_min = UINT64_MAX;
	uint64_t work_max = 0;
	uint64_t work_sum = 0;
	uint64_t next_ns;
	unsigned int duration;
	struct sched_param parameters;
	struct timespec now;
	struct timespec resolution;
	int priority;
	unsigned int index;

	if (argc != 2 || parse_duration(argv[1], &duration) != 0) {
		fprintf(stderr, "usage: linux-rt-latency SECONDS (1..3600)\n");
		return EXIT_FAILURE;
	}
	priority = sched_get_priority_max(SCHED_FIFO);
	if (priority < 1) {
		perror("linux-rt-latency: sched_get_priority_max");
		return EXIT_FAILURE;
	}
	memset(&parameters, 0, sizeof(parameters));
	parameters.sched_priority = priority;
	if (sched_setscheduler(0, SCHED_FIFO, &parameters) != 0) {
		perror("linux-rt-latency: sched_setscheduler(SCHED_FIFO)");
		return EXIT_FAILURE;
	}
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 ||
	    clock_getres(CLOCK_MONOTONIC, &resolution) != 0) {
		perror("linux-rt-latency: CLOCK_MONOTONIC");
		return EXIT_FAILURE;
	}
	next_ns = timespec_ns(&now);
	next_ns = ((next_ns / PERIOD_NS) + 1ULL) * PERIOD_NS;
	planned_releases = (uint64_t)duration * 1000ULL;

	while (releases < planned_releases) {
		struct timespec deadline = ns_timespec(next_ns);
		uint64_t now_ns;
		uint64_t dispatch_ns;
		uint64_t elapsed_periods;
		uint64_t remaining;
		uint64_t finish_ns;
		uint64_t work_ns;
		uint32_t state;
		int result;

		do {
			result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME,
						 &deadline, NULL);
		} while (result == EINTR);
		if (result != 0) {
			errno = result;
			perror("linux-rt-latency: clock_nanosleep");
			return EXIT_FAILURE;
		}
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
			perror("linux-rt-latency: clock_gettime");
			return EXIT_FAILURE;
		}
		now_ns = timespec_ns(&now);
		dispatch_ns = now_ns > next_ns ? now_ns - next_ns : 0;
		elapsed_periods = dispatch_ns / PERIOD_NS + 1ULL;
		remaining = planned_releases - releases;
		if (elapsed_periods > remaining)
			elapsed_periods = remaining;
		releases += elapsed_periods;
		missed += elapsed_periods - 1ULL;
		executions++;

		if (dispatch_ns < dispatch_min)
			dispatch_min = dispatch_ns;
		if (dispatch_ns > dispatch_max)
			dispatch_max = dispatch_ns;
		dispatch_sum += dispatch_ns;
		dispatch_histogram[histogram_bin(dispatch_ns)]++;

		state = payload_state;
		for (index = 0; index < WORK_ITERATIONS; ++index) {
			state = state * 1664525U + 1013904223U;
			__asm__ volatile("" : "+r"(state));
		}
		payload_state = state;
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
			perror("linux-rt-latency: finish clock_gettime");
			return EXIT_FAILURE;
		}
		finish_ns = timespec_ns(&now);
		work_ns = finish_ns > now_ns ? finish_ns - now_ns : 0;
		if (work_ns < work_min)
			work_min = work_ns;
		if (work_ns > work_max)
			work_max = work_ns;
		work_sum += work_ns;
		if (finish_ns >= next_ns + PERIOD_NS)
			late_finishes++;
		next_ns += elapsed_periods * PERIOD_NS;
	}

	printf("{\"measurement\":\"timer-to-sched-fifo\","
	       "\"physical_reference\":false,\"clock\":\"CLOCK_MONOTONIC\","
	       "\"period_ns\":%llu,\"work_iterations\":%u,"
	       "\"scheduler\":\"SCHED_FIFO\",\"priority\":%d,"
	       "\"duration_s\":%u,\"clock_resolution_ns\":%llu,"
	       "\"planned_releases\":%llu,\"releases\":%llu,"
	       "\"executions\":%llu,\"missed_releases\":%llu,"
	       "\"late_finishes\":%llu,"
	       "\"dispatch_ns\":{\"min\":%llu,\"average\":%llu,"
	       "\"p99_upper_us\":%u,\"p99_9_upper_us\":%u,\"max\":%llu},"
	       "\"work_ns\":{\"min\":%llu,\"average\":%llu,\"max\":%llu},"
	       "\"dispatch_histogram\":[",
	       (unsigned long long)PERIOD_NS, WORK_ITERATIONS, priority, duration,
	       (unsigned long long)timespec_ns(&resolution),
	       (unsigned long long)planned_releases, (unsigned long long)releases,
	       (unsigned long long)executions, (unsigned long long)missed,
	       (unsigned long long)late_finishes,
	       (unsigned long long)dispatch_min,
	       (unsigned long long)(dispatch_sum / executions),
	       percentile_upper(dispatch_histogram, executions, 990),
	       percentile_upper(dispatch_histogram, executions, 999),
	       (unsigned long long)dispatch_max, (unsigned long long)work_min,
	       (unsigned long long)(work_sum / executions),
	       (unsigned long long)work_max);
	for (index = 0; index < HISTOGRAM_BINS; ++index) {
		printf("%s{\"upper_us\":%u,\"count\":%llu}",
		       index == 0 ? "" : ",", histogram_upper_us[index],
		       (unsigned long long)dispatch_histogram[index]);
	}
	printf("]}\n");
	return releases == planned_releases ? EXIT_SUCCESS : EXIT_FAILURE;
}
