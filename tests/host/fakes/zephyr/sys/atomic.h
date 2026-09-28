/* Host-test fake for <zephyr/sys/atomic.h> -- single-threaded tests, so
 * plain loads/stores with Zephyr's names and semantics (atomic_cas returns
 * true on success, like the real one).
 */
#ifndef FAKE_ZEPHYR_SYS_ATOMIC_H_
#define FAKE_ZEPHYR_SYS_ATOMIC_H_

#include <stdbool.h>

typedef long atomic_t;
typedef long atomic_val_t;

#define ATOMIC_INIT(v) (v)

static inline atomic_val_t atomic_get(const atomic_t *target)
{
	return *target;
}

static inline atomic_val_t atomic_set(atomic_t *target, atomic_val_t value)
{
	atomic_val_t old = *target;

	*target = value;
	return old;
}

static inline bool atomic_cas(atomic_t *target, atomic_val_t old_value, atomic_val_t new_value)
{
	if (*target == old_value) {
		*target = new_value;
		return true;
	}
	return false;
}

#endif /* FAKE_ZEPHYR_SYS_ATOMIC_H_ */
