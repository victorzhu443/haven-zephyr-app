/* Host-test fake for <zephyr/kernel.h>.
 *
 * Provides just enough of the k_work/K_MSEC/sleep/uptime surface for
 * mock_audio_pipeline.c and adau1860_control.c to compile unmodified under
 * host gcc. The work-queue pieces are link-satisfying stubs (the tests call
 * the production static functions directly, never the scheduler path).
 * k_uptime_get() does advance -- adau1860_control.c's STATUS2 poll uses it
 * for a timeout, and a clock that never moves would turn a failing poll into
 * an infinite loop in the test instead of a clean -ETIMEDOUT.
 */
#ifndef FAKE_ZEPHYR_KERNEL_H_
#define FAKE_ZEPHYR_KERNEL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef __packed
#define __packed __attribute__((__packed__))
#endif

#define ARG_UNUSED(x) ((void)(x))

struct k_work;
typedef void (*k_work_handler_t)(struct k_work *work);

struct k_work {
	k_work_handler_t handler;
};

struct k_work_delayable {
	struct k_work work;
};

typedef struct { int64_t ticks; } k_timeout_t;

#define K_MSEC(ms) ((k_timeout_t){ .ticks = (ms) })

#define K_WORK_DELAYABLE_DEFINE(name, work_handler) \
	struct k_work_delayable name = { .work = { .handler = (work_handler) } }

static inline struct k_work_delayable *k_work_delayable_from_work(struct k_work *work)
{
	return (struct k_work_delayable *)work;
}

static inline int k_work_schedule(struct k_work_delayable *dwork, k_timeout_t delay)
{
	ARG_UNUSED(dwork);
	ARG_UNUSED(delay);
	return 0;
}

static inline int k_work_reschedule(struct k_work_delayable *dwork, k_timeout_t delay)
{
	ARG_UNUSED(dwork);
	ARG_UNUSED(delay);
	return 0;
}

static inline int k_work_cancel_delayable(struct k_work_delayable *dwork)
{
	ARG_UNUSED(dwork);
	return 0;
}

static inline bool device_is_ready(const void *dev)
{
	ARG_UNUSED(dev);
	return true;
}

/* Simulated clock: every sleep advances it. */
static int64_t haven_fake_uptime_ms;

static inline int64_t k_uptime_get(void)
{
	return haven_fake_uptime_ms;
}

static inline int32_t k_msleep(int32_t ms)
{
	haven_fake_uptime_ms += ms;
	return 0;
}

static inline int32_t k_usleep(int32_t us)
{
	haven_fake_uptime_ms += (us + 999) / 1000;
	return 0;
}

#endif /* FAKE_ZEPHYR_KERNEL_H_ */
