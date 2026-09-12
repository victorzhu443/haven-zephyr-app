/* Host-test fake for <zephyr/drivers/gpio.h> -- link-satisfying stubs that
 * record the enable pin's last requested state, nothing more.
 */
#ifndef FAKE_ZEPHYR_DRIVERS_GPIO_H_
#define FAKE_ZEPHYR_DRIVERS_GPIO_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>

typedef uint32_t gpio_flags_t;
typedef uint8_t gpio_pin_t;

#define GPIO_OUTPUT_ACTIVE   0x01u
#define GPIO_OUTPUT_INACTIVE 0x02u
#define GPIO_ACTIVE_HIGH     0x00u

struct gpio_dt_spec {
	const struct device *port;
	gpio_pin_t pin;
	gpio_flags_t dt_flags;
};

extern const struct device haven_fake_gpio_port_dev;
#define GPIO_DT_SPEC_GET(node, prop) { .port = &haven_fake_gpio_port_dev, .pin = 4, .dt_flags = 0 }

static int haven_fake_gpio_configure_calls;
static gpio_flags_t haven_fake_gpio_last_flags;

static inline bool gpio_is_ready_dt(const struct gpio_dt_spec *spec)
{
	(void)spec;
	return true;
}

static inline int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, gpio_flags_t extra_flags)
{
	(void)spec;
	haven_fake_gpio_configure_calls++;
	haven_fake_gpio_last_flags = extra_flags;
	return 0;
}

static inline int gpio_pin_set_dt(const struct gpio_dt_spec *spec, int value)
{
	(void)spec;
	(void)value;
	return 0;
}

#endif /* FAKE_ZEPHYR_DRIVERS_GPIO_H_ */
