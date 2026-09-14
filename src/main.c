/* Haven nRF5340 firmware
 *
 * Pipeline: NUS write → line assembler → protocol_parse_line() → ADAU1860
 * driver. Parsing and DSP dispatch run on the system workqueue via the BLE
 * receive path; nothing here blocks.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "adau1860_control.h"
#include "ble_transport.h"
#include "gatt_audio_service.h"
#include "mock_audio_pipeline.h"
#include "protocol.h"
#include "settings_store.h"
#include "tone_gen.h"
#include "tone_safety.h"
#include "wake_button.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static void handle_line(const char *line)
{
	struct dsp_command cmd;
	int err = protocol_parse_line(line, &cmd);

	if (err) {
		LOG_WRN("Rejected payload: %s", line);
		return;
	}

	switch (cmd.type) {
	case DSP_CMD_MULTI_FILTER:
		LOG_INF("MULTI_FILTER: %u band(s)", cmd.band_count);
		adau1860_control_set_bypass(false);
		adau1860_control_apply_filters(cmd.bands, cmd.band_count);
		break;
	case DSP_CMD_BYPASS:
		adau1860_control_set_bypass(cmd.bypass_enabled);
		break;
	case DSP_CMD_TONE_START:
		tone_safety_start(cmd.tone_f0_hz, cmd.tone_level_db);
		break;
	case DSP_CMD_TONE_LEVEL:
		tone_safety_set_level(cmd.tone_level_db);
		break;
	case DSP_CMD_TONE_STOP:
		tone_safety_stop();
		break;
	default:
		break;
	}
}

/* Losing the BLE link must silence any active tone immediately, the same
 * way haven-app's own useLdlTone does on its side (docs/safety.md) --
 * independently, not because the app told us to. A plain wrapper here
 * (rather than teaching adau1860_control.c about tone_safety.c) keeps that
 * module's only dependency on tone_safety.h one-directional.
 */
static void on_ble_disconnected(void)
{
	adau1860_control_on_ble_disconnected();
	tone_safety_stop();
}

int main(void)
{
	LOG_INF("Haven firmware boot");

	int err = adau1860_control_init();

	if (err) {
		LOG_ERR("ADAU1860 control init failed (err %d)", err);
	}

	/* nRF-side LDL tone generator (I2S0 master). Independent of the codec
	 * init above: on a DK without a codec it still streams, which is how
	 * the I2S clocking gets checked with a scope. */
	err = tone_gen_init();
	if (err) {
		LOG_WRN("Tone generator init failed (err %d) -- TONE_* will be no-ops", err);
	}

	ble_transport_set_conn_callbacks(adau1860_control_on_ble_connected,
					  on_ble_disconnected);

	err = ble_transport_init(handle_line);
	if (err) {
		LOG_ERR("BLE init failed (err %d)", err);
		return err;
	}

	/* BT_GATT_SERVICE_DEFINE registers the service automatically inside
	 * bt_enable() (called from ble_transport_init() above); this just
	 * confirms it and logs the bench-default parameter values.
	 */
	gatt_audio_service_init();
	mock_audio_pipeline_init();

	/* After mock_audio_pipeline_init() so a restored value's callback
	 * (registered by that call) actually fires -- otherwise the pipeline
	 * would start from its own hardcoded defaults instead of whatever
	 * was last saved.
	 */
	haven_settings_init();

	err = wake_button_init();
	if (err) {
		LOG_WRN("Wake button init failed (err %d) -- continuing without it", err);
	}

	LOG_INF("Ready — waiting for app connection");
	return 0;
}
