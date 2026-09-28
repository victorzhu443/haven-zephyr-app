/* Host-test fake for <zephyr/sys/util.h> -- the few helpers the production
 * sources use, with their real semantics.
 */
#ifndef FAKE_ZEPHYR_SYS_UTIL_H_
#define FAKE_ZEPHYR_SYS_UTIL_H_

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif
#ifndef ROUND_UP
#define ROUND_UP(x, align) ((((x) + (align) - 1) / (align)) * (align))
#endif
#ifndef MIN
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#endif

#endif /* FAKE_ZEPHYR_SYS_UTIL_H_ */
