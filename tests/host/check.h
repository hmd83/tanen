/* Minimal assertion harness. No framework dependency on purpose: the suite has
 * to build with nothing but a host C compiler, on any CI runner or laptop.
 * A failed check reports and keeps going, so one run shows every failure. */
#ifndef TANEN_HOST_CHECK_H
#define TANEN_HOST_CHECK_H

#include <inttypes.h>
#include <stdio.h>

static int check_failures;
static int check_count;

#define CHECK(cond) do {                                                     \
	check_count++;                                                        \
	if (!(cond)) {                                                        \
		check_failures++;                                             \
		fprintf(stderr, "%s:%d: CHECK failed: %s\n",                  \
			__FILE__, __LINE__, #cond);                           \
	}                                                                     \
} while (0)

#define CHECK_EQ(a, b) do {                                                  \
	int64_t check_a_ = (int64_t)(a), check_b_ = (int64_t)(b);             \
	check_count++;                                                        \
	if (check_a_ != check_b_) {                                           \
		check_failures++;                                             \
		fprintf(stderr, "%s:%d: CHECK_EQ failed: %s == %s "           \
			"(%" PRId64 " != %" PRId64 ")\n", __FILE__, __LINE__, \
			#a, #b, check_a_, check_b_);                          \
	}                                                                     \
} while (0)

#define RUN(test) do {                                                       \
	int check_before_ = check_failures;                                   \
	test();                                                               \
	printf("%-4s %s\n", check_failures == check_before_ ? "ok" : "FAIL",  \
	       #test);                                                        \
} while (0)

#define CHECK_SUMMARY() (printf("%d checks, %d failed\n",                    \
				check_count, check_failures),                 \
			 check_failures ? 1 : 0)

#endif
