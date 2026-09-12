/* OpenEarable 2.0 / Haven board: turn on the load-switched rails at boot.
 *
 * Simplified from upstream OpenEarable's board_init.c (Apache-2.0): upstream
 * wraps each rail in a PM device so subsystems can reference-count it; Haven
 * has one consumer that must be on for the product to function at all (the
 * ADAU1860 + PDM mic sit on V_LS behind the 1.8 V load switch, P1.11), so the
 * rails are simply driven on before drivers initialise.
 *
 *   load_switch     (P1.11, AP22916): V_LS -> codec, mic, flex connector
 *   bq25120a/load-switch (P0.14):     3.3 V rail (sensors etc.)
 */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(board_init, LOG_LEVEL_INF);

#define LS_1_8_NODE DT_NODELABEL(load_switch)
#define LS_3_3_NODE DT_CHILD(DT_NODELABEL(bq25120a), load_switch)

static const struct gpio_dt_spec ls_1_8 = GPIO_DT_SPEC_GET(LS_1_8_NODE, enable_gpios);
static const struct gpio_dt_spec ls_3_3 = GPIO_DT_SPEC_GET(LS_3_3_NODE, enable_gpios);

static int rail_on(const struct gpio_dt_spec *ls, int delay_us, const char *name)
{
	if (!gpio_is_ready_dt(ls)) {
		LOG_ERR("%s load switch GPIO not ready", name);
		return -ENODEV;
	}
	int err = gpio_pin_configure_dt(ls, GPIO_OUTPUT_ACTIVE);

	if (err) {
		LOG_ERR("%s load switch: %d", name, err);
		return err;
	}
	k_busy_wait(delay_us);
	return 0;
}

static int openearable_rails_init(void)
{
	int err = rail_on(&ls_1_8, DT_PROP(LS_1_8_NODE, power_delay_us), "1.8V (V_LS)");

	if (err) {
		return err;
	}
	return rail_on(&ls_3_3, DT_PROP(LS_3_3_NODE, power_delay_us), "3.3V");
}

SYS_INIT(openearable_rails_init, POST_KERNEL, 80);
