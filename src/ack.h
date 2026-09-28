/* Device -> app replies over NUS TX.
 *
 * Every line the app writes gets exactly one newline-terminated JSON ack
 * back, and a few unsolicited events exist for things the app cannot
 * otherwise observe (the tone watchdog firing; what firmware it just
 * connected to). Pure C: this module only formats bytes into a caller's
 * buffer -- sending is ble_transport_send()'s job, wiring is main.c's -- so
 * the whole schema is host-testable.
 *
 * Schema (see docs/nus-acks.md; keep the two in sync):
 *
 *   {"ack":"MULTI_FILTER","ok":true,"bands":3}\n
 *   {"ack":"BYPASS","ok":true,"enabled":true}\n
 *   {"ack":"TONE_START","ok":true,"f0":4000,"level_db":30}\n
 *   {"ack":"TONE_LEVEL","ok":true,"level_db":42}\n
 *   {"ack":"TONE_STOP","ok":true}\n
 *   {"ack":"?","ok":false,"err":"parse"}\n            rejected line
 *   {"ack":"MULTI_FILTER","ok":false,"err":"dsp","code":-5}\n
 *   {"event":"tone_watchdog"}\n                        firmware auto-silenced
 *   {"event":"boot","fw":"0.1.0-dev","fdsp_rate":192000,"dac_source":"fdsp"}\n
 *
 * Numbers are integers (f0 and level_db are rounded) so no float formatting
 * is needed on the wire, and every message fits comfortably inside one
 * NUS notification at the app's negotiated MTU (247 - 3 = 244 bytes; the
 * longest message here is well under 100).
 */
#ifndef HAVEN_ACK_H_
#define HAVEN_ACK_H_

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

/* Largest formatted line including the trailing '\n' and NUL. */
#define ACK_MAX_LEN 120

/* Ack for a successfully parsed and dispatched command. `dispatch_err` is
 * the driver's return code (0 = applied); nonzero produces an ok:false ack
 * with err:"dsp" and the code. Returns the number of bytes written
 * (excluding the NUL), or a negative errno if `size` is too small.
 */
int ack_format_command(const struct dsp_command *cmd, int dispatch_err, char *buf,
		       size_t size);

/* Ack for a line protocol_parse_line() rejected. */
int ack_format_rejected(char *buf, size_t size);

/* Unsolicited events. */
int ack_format_event_tone_watchdog(char *buf, size_t size);
int ack_format_event_boot(const char *fw_version, uint32_t fdsp_rate_hz,
			  const char *dac_source, char *buf, size_t size);

#endif /* HAVEN_ACK_H_ */
