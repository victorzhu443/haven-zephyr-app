/* Host-test fake for <zephyr/device.h> + the slice of <zephyr/devicetree.h>
 * adau1860_control.c uses. DT_NODELABEL() collapses to a plain token; the
 * ADAU1860 node is modelled as having enable-gpios but no supply-gpios, so
 * the #if DT_NODE_HAS_PROP(..., supply_gpios) branch compiles out exactly as
 * it does on the nRF5340 DK overlay.
 */
#ifndef FAKE_ZEPHYR_DEVICE_H_
#define FAKE_ZEPHYR_DEVICE_H_

#include <stdbool.h>

struct device {
	const char *name;
};

#define DT_NODELABEL(label) label
#define DT_NODE_HAS_PROP(node, prop) 0
#define DT_PROP(node, prop) 0

#endif /* FAKE_ZEPHYR_DEVICE_H_ */
