/* Host tests for the device -> app ack/event schema (ack.c) and the tone
 * watchdog notification hook (tone_safety.c). Both production files are
 * included as-is; ack.c has no Zephyr dependency, tone_safety.c uses only
 * the k_work fake. The three adau1860_control_* tone functions tone_safety.c
 * calls are stubbed here so the watchdog path can run without the codec
 * driver (which has its own suite).
 */
#include "test_harness.h"

#include <errno.h>
#include <string.h>

#include "../../src/ack.c"
#include "../../src/protocol.c"
#include "../../src/tone_safety.c"

/* ── stubs for tone_safety.c's codec calls ─────────────────────────────── */
static int stub_stop_calls;
int adau1860_control_set_tone(float f0_hz, float level_db)
{
	(void)f0_hz;
	(void)level_db;
	return 0;
}
int adau1860_control_set_tone_level(float level_db)
{
	(void)level_db;
	return 0;
}
int adau1860_control_stop_tone(void)
{
	stub_stop_calls++;
	return 0;
}

/* ── helpers ────────────────────────────────────────────────────────────── */
static int parse(const char *line, struct dsp_command *cmd)
{
	return protocol_parse_line(line, cmd);
}

static int ends_with_newline_only(const char *s, int n)
{
	return n > 0 && s[n - 1] == '\n' && strchr(s, '\n') == s + n - 1;
}

/* ── tests ──────────────────────────────────────────────────────────────── */

static void test_ack_multi_filter(void)
{
	struct dsp_command cmd;
	char buf[ACK_MAX_LEN];

	CHECK(parse("{\"type\":\"MULTI_FILTER\",\"bands\":[{\"f0\":4500,\"Q\":10},{\"f0\":1000,\"Q\":5},{\"f0\":2000,\"Q\":5}]}",
		    &cmd) == 0);
	int n = ack_format_command(&cmd, 0, buf, sizeof(buf));

	CHECK(n > 0);
	CHECK(strcmp(buf, "{\"ack\":\"MULTI_FILTER\",\"ok\":true,\"bands\":3}\n") == 0);
	CHECK((int)strlen(buf) == n);
	CHECK(ends_with_newline_only(buf, n));
}

static void test_ack_bypass_both_values(void)
{
	struct dsp_command cmd;
	char buf[ACK_MAX_LEN];

	CHECK(parse("{\"type\":\"BYPASS\",\"enabled\":true}", &cmd) == 0);
	CHECK(ack_format_command(&cmd, 0, buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "{\"ack\":\"BYPASS\",\"ok\":true,\"enabled\":true}\n") == 0);

	CHECK(parse("{\"type\":\"BYPASS\",\"enabled\":false}", &cmd) == 0);
	CHECK(ack_format_command(&cmd, 0, buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "{\"ack\":\"BYPASS\",\"ok\":true,\"enabled\":false}\n") == 0);
}

static void test_ack_tone_start_echoes_clamped_values_as_integers(void)
{
	struct dsp_command cmd;
	char buf[ACK_MAX_LEN];

	/* level 120 is clamped by protocol.c to 85: the ack must echo what the
	 * device actually applied, not what the app asked for. */
	CHECK(parse("{\"type\":\"TONE_START\",\"f0\":4000.4,\"level_db\":120}", &cmd) == 0);
	CHECK(ack_format_command(&cmd, 0, buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "{\"ack\":\"TONE_START\",\"ok\":true,\"f0\":4000,\"level_db\":85}\n") == 0);
}

static void test_ack_tone_level_and_stop(void)
{
	struct dsp_command cmd;
	char buf[ACK_MAX_LEN];

	CHECK(parse("{\"type\":\"TONE_LEVEL\",\"level_db\":42}", &cmd) == 0);
	CHECK(ack_format_command(&cmd, 0, buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "{\"ack\":\"TONE_LEVEL\",\"ok\":true,\"level_db\":42}\n") == 0);

	CHECK(parse("{\"type\":\"TONE_STOP\"}", &cmd) == 0);
	CHECK(ack_format_command(&cmd, 0, buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "{\"ack\":\"TONE_STOP\",\"ok\":true}\n") == 0);
}

static void test_ack_dispatch_error_reports_code(void)
{
	struct dsp_command cmd;
	char buf[ACK_MAX_LEN];

	CHECK(parse("{\"type\":\"MULTI_FILTER\",\"bands\":[{\"f0\":4500,\"Q\":10}]}", &cmd) == 0);
	CHECK(ack_format_command(&cmd, -EIO, buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "{\"ack\":\"MULTI_FILTER\",\"ok\":false,\"err\":\"dsp\",\"code\":-5}\n") == 0);
}

static void test_ack_rejected_line(void)
{
	char buf[ACK_MAX_LEN];
	int n = ack_format_rejected(buf, sizeof(buf));

	CHECK(n > 0);
	CHECK(strcmp(buf, "{\"ack\":\"?\",\"ok\":false,\"err\":\"parse\"}\n") == 0);
}

static void test_events(void)
{
	char buf[ACK_MAX_LEN];

	CHECK(ack_format_event_tone_watchdog(buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf, "{\"event\":\"tone_watchdog\"}\n") == 0);

	CHECK(ack_format_event_boot("0.1.0-dev", 192000, "fdsp", buf, sizeof(buf)) > 0);
	CHECK(strcmp(buf,
		     "{\"event\":\"boot\",\"fw\":\"0.1.0-dev\",\"fdsp_rate\":192000,\"dac_source\":\"fdsp\"}\n") == 0);
}

/* Every message must fit one NUS notification at the app's negotiated MTU
 * (247): ATT payload is MTU - 3 = 244 bytes. ACK_MAX_LEN is the documented
 * bound; prove both that the bound holds for the worst case and that a
 * too-small buffer is refused rather than truncated.
 */
static void test_length_bounds(void)
{
	struct dsp_command cmd;
	char buf[ACK_MAX_LEN];
	int worst = 0, n;

	CHECK(parse("{\"type\":\"MULTI_FILTER\",\"bands\":[{\"f0\":8000,\"Q\":20},{\"f0\":8000,\"Q\":20},{\"f0\":8000,\"Q\":20},{\"f0\":8000,\"Q\":20},{\"f0\":8000,\"Q\":20}]}",
		    &cmd) == 0);
	n = ack_format_command(&cmd, -2147483647, buf, sizeof(buf)); worst = n > worst ? n : worst;
	CHECK(parse("{\"type\":\"TONE_START\",\"f0\":8000,\"level_db\":85}", &cmd) == 0);
	n = ack_format_command(&cmd, 0, buf, sizeof(buf)); worst = n > worst ? n : worst;
	n = ack_format_event_boot("99.99.99-averyveryverylongextraversion", 192000, "dmic_direct", buf,
				  sizeof(buf)); worst = n > worst ? n : worst;

	CHECK(worst > 0);
	CHECK(worst < ACK_MAX_LEN);
	CHECK(worst <= 244);

	char tiny[8];

	CHECK(ack_format_rejected(tiny, sizeof(tiny)) == -ENOSPC);
	CHECK(ack_format_event_tone_watchdog(tiny, sizeof(tiny)) == -ENOSPC);
}

/* ── tone_safety watchdog hook ──────────────────────────────────────────── */
static int watchdog_events;
static void on_watchdog(void)
{
	watchdog_events++;
	/* The callback runs after the stop: the tone is already silent. */
	CHECK(tone_active == false);
}

static void test_watchdog_fires_callback_after_silencing(void)
{
	tone_safety_set_watchdog_cb(on_watchdog);
	stub_stop_calls = 0;
	watchdog_events = 0;

	tone_safety_start(4000.0f, 30.0f);
	CHECK(tone_active == true);

	/* Fire the delayable work by hand (the fake kernel never runs it). */
	watchdog.work.handler(&watchdog.work);

	CHECK(stub_stop_calls == 1);
	CHECK(watchdog_events == 1);
	CHECK(tone_active == false);

	/* An explicit stop is silent: no event, no second codec stop. */
	tone_safety_start(4000.0f, 30.0f);
	tone_safety_stop();
	CHECK(watchdog_events == 1);

	/* Watchdog with no active tone: stop is a no-op, event still reported
	 * (the app learns the watchdog ran even if nothing was playing). */
	watchdog.work.handler(&watchdog.work);
	CHECK(stub_stop_calls == 2);
	CHECK(watchdog_events == 2);

	/* No callback registered: silencing still happens. */
	tone_safety_set_watchdog_cb(NULL);
	tone_safety_start(4000.0f, 30.0f);
	watchdog.work.handler(&watchdog.work);
	CHECK(tone_active == false);
	CHECK(watchdog_events == 2);
}

int main(void)
{
	RUN(test_ack_multi_filter);
	RUN(test_ack_bypass_both_values);
	RUN(test_ack_tone_start_echoes_clamped_values_as_integers);
	RUN(test_ack_tone_level_and_stop);
	RUN(test_ack_dispatch_error_reports_code);
	RUN(test_ack_rejected_line);
	RUN(test_events);
	RUN(test_length_bounds);
	RUN(test_watchdog_fires_callback_after_silencing);
	return haven_test_summary("test_ack");
}
