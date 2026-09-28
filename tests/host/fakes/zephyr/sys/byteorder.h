/* Host-test fake for <zephyr/sys/byteorder.h> -- real pack/unpack
 * semantics (not no-ops): gatt_audio_service.c's FreqRange wire format and
 * adau1860_control.c's register framing (big-endian address, little-endian
 * memory words) both depend on these doing exactly what they say.
 */
#ifndef FAKE_ZEPHYR_SYS_BYTEORDER_H_
#define FAKE_ZEPHYR_SYS_BYTEORDER_H_

#include <stdint.h>

static inline void sys_put_le16(uint16_t val, uint8_t dst[2])
{
	dst[0] = (uint8_t)(val & 0xff);
	dst[1] = (uint8_t)((val >> 8) & 0xff);
}

static inline uint16_t sys_get_le16(const uint8_t src[2])
{
	return (uint16_t)(src[0] | ((uint16_t)src[1] << 8));
}

static inline void sys_put_le32(uint32_t val, uint8_t dst[4])
{
	dst[0] = (uint8_t)(val & 0xff);
	dst[1] = (uint8_t)((val >> 8) & 0xff);
	dst[2] = (uint8_t)((val >> 16) & 0xff);
	dst[3] = (uint8_t)((val >> 24) & 0xff);
}

static inline uint32_t sys_get_le32(const uint8_t src[4])
{
	return (uint32_t)src[0] | ((uint32_t)src[1] << 8) | ((uint32_t)src[2] << 16) |
	       ((uint32_t)src[3] << 24);
}

static inline void sys_put_be32(uint32_t val, uint8_t dst[4])
{
	dst[0] = (uint8_t)((val >> 24) & 0xff);
	dst[1] = (uint8_t)((val >> 16) & 0xff);
	dst[2] = (uint8_t)((val >> 8) & 0xff);
	dst[3] = (uint8_t)(val & 0xff);
}

static inline uint32_t sys_get_be32(const uint8_t src[4])
{
	return ((uint32_t)src[0] << 24) | ((uint32_t)src[1] << 16) | ((uint32_t)src[2] << 8) |
	       (uint32_t)src[3];
}

#endif /* FAKE_ZEPHYR_SYS_BYTEORDER_H_ */
