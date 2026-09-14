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

#define K_FOREVER ((k_timeout_t){ .ticks = -1 })
#define K_NO_WAIT ((k_timeout_t){ .ticks = 0 })

/* -- Semaphores / mutexes: single-threaded tests, so counters only. -- */
struct k_sem {
	unsigned int count;
	unsigned int limit;
};
/* Like Zephyr's, no storage class of its own: production code writes
 * `static K_SEM_DEFINE(...)` and that must stay legal here. */
#define K_SEM_DEFINE(name, initial, lim) \
	struct k_sem name = { .count = (initial), .limit = (lim) }

static inline void k_sem_give(struct k_sem *sem)
{
	if (sem->count < sem->limit) {
		sem->count++;
	}
}

static inline int k_sem_take(struct k_sem *sem, k_timeout_t timeout)
{
	ARG_UNUSED(timeout);
	if (sem->count == 0) {
		return -11; /* -EAGAIN: nothing to take, never blocks in tests */
	}
	sem->count--;
	return 0;
}

struct k_mutex {
	int lock_count;
};
#define K_MUTEX_DEFINE(name) struct k_mutex name = { .lock_count = 0 }

static inline int k_mutex_lock(struct k_mutex *m, k_timeout_t timeout)
{
	ARG_UNUSED(timeout);
	m->lock_count++;
	return 0;
}

static inline int k_mutex_unlock(struct k_mutex *m)
{
	m->lock_count--;
	return 0;
}

/* -- Memory slab: a real fixed-block allocator so alloc/free pairing in the
 * code under test is checked, not waved through. -- */
#define HAVEN_FAKE_SLAB_MAX_BLOCKS 16

struct k_mem_slab {
	uint8_t *buf;
	size_t block_size;
	unsigned int num_blocks;
	unsigned int used[HAVEN_FAKE_SLAB_MAX_BLOCKS];
	unsigned int num_used;
};

#define K_MEM_SLAB_DEFINE_STATIC(name, slab_block_size, slab_num_blocks, slab_align) \
	static uint8_t name##_fake_buf[(slab_block_size) * (slab_num_blocks)] \
		__attribute__((aligned(slab_align))); \
	static struct k_mem_slab name = { .buf = name##_fake_buf, \
					  .block_size = (slab_block_size), \
					  .num_blocks = (slab_num_blocks) }

static inline int k_mem_slab_alloc(struct k_mem_slab *slab, void **mem, k_timeout_t timeout)
{
	ARG_UNUSED(timeout);
	for (unsigned int i = 0; i < slab->num_blocks && i < HAVEN_FAKE_SLAB_MAX_BLOCKS; i++) {
		if (!slab->used[i]) {
			slab->used[i] = 1;
			slab->num_used++;
			*mem = slab->buf + i * slab->block_size;
			return 0;
		}
	}
	return -12; /* -ENOMEM: exhausted (a leak in the code under test) */
}

static inline void k_mem_slab_free(struct k_mem_slab *slab, void *mem)
{
	size_t off = (size_t)((uint8_t *)mem - slab->buf);
	unsigned int i = (unsigned int)(off / slab->block_size);

	if (i < slab->num_blocks && slab->used[i]) {
		slab->used[i] = 0;
		slab->num_used--;
	}
}

/* -- Threads: creation is recorded, never run (tests drive the loop bodies
 * directly). -- */
typedef unsigned long k_thread_stack_t;
#define K_THREAD_STACK_DEFINE(name, size) static k_thread_stack_t name[((size) + 7) / 8]
#define K_THREAD_STACK_SIZEOF(name) sizeof(name)
#define K_PRIO_PREEMPT(p) (p)

struct k_thread {
	const char *name;
};
typedef struct k_thread *k_tid_t;
typedef void (*k_thread_entry_t)(void *, void *, void *);

static int haven_fake_threads_created;

static inline k_tid_t k_thread_create(struct k_thread *t, k_thread_stack_t *stack, size_t stack_size,
				      k_thread_entry_t entry, void *p1, void *p2, void *p3, int prio,
				      uint32_t options, k_timeout_t delay)
{
	ARG_UNUSED(stack);
	ARG_UNUSED(stack_size);
	ARG_UNUSED(entry);
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	ARG_UNUSED(prio);
	ARG_UNUSED(options);
	ARG_UNUSED(delay);
	haven_fake_threads_created++;
	return t;
}

static inline int k_thread_name_set(k_tid_t t, const char *name)
{
	t->name = name;
	return 0;
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
