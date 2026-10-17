/* Host tests for src/core/fsm.c — state transitions with every collaborator
 * faked, power_get_wake_reason() included.
 *
 * Built with the feature switches prj.conf resolves to (tempcomp, BLE config
 * and Extended Mode all on). Each fake appends its name to a call trace, so the
 * tests can assert ORDER where the design depends on it: the BLE scan must
 * finish before lora_init(), the tempcomp state must be saved before System OFF.
 */
#include <stdio.h>
#include <string.h>

#define CONFIG_TANENBASE_TEMPCOMP 1
#define CONFIG_TANENBASE_BLE_CONFIG 1
#define CONFIG_TANENBASE_EXT_SENSORS 1
#define CONFIG_TANENBASE_SETUP_TIMEOUT 180
#define CONFIG_TANENBASE_EXT_SCAN_TIMEOUT_MS 8000
#define CONFIG_TANENBASE_DEBUG_LOOP_INTERVAL 180

#include "../../src/core/fsm.c"

#include "check.h"

/* --- call trace -------------------------------------------------------- */

static char trace[1024];

static void called(const char *name)
{
	strncat(trace, name, sizeof(trace) - strlen(trace) - 2);
	strcat(trace, " ");
}

/* Position of a call in the trace, or -1. */
static int at(const char *name)
{
	char key[64];

	snprintf(key, sizeof(key), "%s ", name);
	const char *p = strstr(trace, key);

	return p ? (int)(p - trace) : -1;
}

/* --- fakes ------------------------------------------------------------- */

int64_t stub_uptime_ms;
bool fake_led_ready;
int  fake_led_level;
int  fake_led_configure_err;

static wake_reason_t fake_wake;
static int           fake_config_init_err;
static measurement_data_t fake_meas;
static anomaly_result_t   fake_anomaly;
static uint8_t  fake_tx_pending;
static int      fake_tx_pending_writes;
static uint32_t fake_meas_count;
static uint32_t fake_ms_interval;
static bool     fake_ext_active;
static int      fake_lora_init_err;
static int      fake_transmit_ret;
static const ext_data_t *fake_transmit_ext;
static uint8_t  fake_transmit_flags;
static int      fake_power_off_ret;
static uint32_t fake_power_off_s;
static int      fake_ble_timeout_s;
static int      fake_led_during_setup;
static int      fake_wdt_feeds;
static bool     fake_last_w_set, fake_last_t_set;
static uint32_t fake_last_w;
static int16_t  fake_last_t;

wake_reason_t power_get_wake_reason(void) { return fake_wake; }
int power_off(uint32_t s) { called("power_off"); fake_power_off_s = s; return fake_power_off_ret; }
void watchdog_feed(void) { fake_wdt_feeds++; }

int config_init(void) { called("config_init"); return fake_config_init_err; }
int config_get_measurement_count(uint32_t *v) { *v = fake_meas_count; return 0; }
int config_set_measurement_count(uint32_t v) { fake_meas_count = v; return 0; }
int config_get_tx_pending(uint8_t *v) { *v = fake_tx_pending; return 0; }
int config_set_tx_pending(uint8_t v)
{
	fake_tx_pending = v;
	fake_tx_pending_writes++;
	return 0;
}
int config_get_ms_interval(uint32_t *v) { *v = fake_ms_interval; return 0; }
int config_set_last_tx_weight(uint32_t v) { fake_last_w_set = true; fake_last_w = v; return 0; }
int config_set_last_tx_temp(int16_t v) { fake_last_t_set = true; fake_last_t = v; return 0; }
bool config_ext_active(void) { return fake_ext_active; }

int measure_init(void) { called("measure_init"); return 0; }
int measure_run(measurement_data_t *out) { called("measure_run"); *out = fake_meas; return 0; }
int tempcomp_save(void) { called("tempcomp_save"); return 0; }
int weight_sleep(void) { called("weight_sleep"); return 0; }

int anomaly_check(const measurement_data_t *d, anomaly_result_t *r)
{
	(void)d;
	called("anomaly_check");
	*r = fake_anomaly;
	return 0;
}

int ext_scan_once(ext_data_t *out, uint32_t timeout_ms)
{
	(void)timeout_ms;
	called("ext_scan_once");
	memset(out, 0, sizeof(*out));
	return 0;
}

int lora_init(void) { called("lora_init"); return fake_lora_init_err; }
int lora_session_save(void) { called("lora_session_save"); return 0; }

int transmit_run(const measurement_data_t *d, const ext_data_t *ext, uint8_t flags)
{
	(void)d;
	called("transmit_run");
	fake_transmit_ext = ext;
	fake_transmit_flags = flags;
	return fake_transmit_ret;
}

int ble_config_run(uint32_t timeout_s)
{
	called("ble_config_run");
	fake_ble_timeout_s = (int)timeout_s;
	fake_led_during_setup = fake_led_level;
	return 0;
}

/* A healthy unit on a timer wake: valid readings, nothing to send, System OFF
 * succeeds (on the target it never returns; the fake returns 0). */
static void reset_fakes(void)
{
	trace[0] = '\0';
	stub_uptime_ms = 0;
	fake_led_ready = true;
	fake_led_level = -1;
	fake_led_configure_err = 0;
	fake_wake = WAKE_REASON_TIMER;
	fake_config_init_err = 0;
	fake_meas = (measurement_data_t){
		.weight_g = 42000, .temp_cc = 2150, .battery_mv = 4800,
		.valid = MEAS_VALID_WEIGHT | MEAS_VALID_TEMP | MEAS_VALID_BATTERY,
	};
	fake_anomaly = (anomaly_result_t){ 0 };
	fake_tx_pending = 0;
	fake_tx_pending_writes = 0;
	fake_meas_count = 7;
	fake_ms_interval = 900;
	fake_ext_active = false;
	fake_lora_init_err = 0;
	fake_transmit_ret = 0;
	fake_transmit_ext = NULL;
	fake_transmit_flags = 0;
	fake_power_off_ret = 0;
	fake_power_off_s = 0;
	fake_ble_timeout_s = -1;
	fake_led_during_setup = -1;
	fake_wdt_feeds = 0;
	fake_last_w_set = fake_last_t_set = false;
	fake_last_w = 0;
	fake_last_t = 0;
}

/* --- fsm_init ---------------------------------------------------------- */

static void test_init(void)
{
	reset_fakes();
	CHECK_EQ(fsm_init(), 0);
	CHECK_EQ(fake_led_level, -1);  /* configured inactive, never driven */
	CHECK(at("config_init") >= 0);

	/* No LED is fatal: main() sleep-retries rather than run blind. */
	reset_fakes();
	fake_led_ready = false;
	CHECK_EQ(fsm_init(), -ENODEV);

	reset_fakes();
	fake_led_configure_err = -EIO;
	CHECK_EQ(fsm_init(), -EIO);

	/* A config/ZMS failure is not: the unit runs on Kconfig defaults. */
	reset_fakes();
	fake_config_init_err = -EIO;
	CHECK_EQ(fsm_init(), 0);
}

/* --- wake reason -> path ----------------------------------------------- */

/* The common case: measure, persist, no radio, System OFF for ms_interval. */
static void test_timer_wake_measure_only(void)
{
	reset_fakes();
	fsm_run();

	CHECK(at("ble_config_run") < 0);
	CHECK(at("lora_init") < 0);
	CHECK(at("ext_scan_once") < 0);
	CHECK(at("measure_run") >= 0);
	CHECK(at("tempcomp_save") > at("measure_run"));
	CHECK(at("power_off") > at("tempcomp_save"));
	CHECK(at("power_off") > at("weight_sleep"));
	CHECK_EQ(fake_meas_count, 8);
	CHECK_EQ(fake_power_off_s, 900);
	CHECK_EQ(fake_tx_pending_writes, 0);
	CHECK_EQ(fsm_get_state(), FSM_STATE_SLEEP);
}

/* Only the button opens SETUP; it runs BLE with the LED on, then falls through
 * to a normal measurement pass. */
static void test_button_wake_runs_setup_then_measures(void)
{
	reset_fakes();
	fake_wake = WAKE_REASON_BUTTON;
	fsm_run();

	CHECK(at("ble_config_run") >= 0);
	CHECK_EQ(fake_ble_timeout_s, 180);
	CHECK_EQ(fake_led_during_setup, 1);
	CHECK(at("measure_run") > at("ble_config_run"));
	CHECK(at("power_off") >= 0);
	CHECK_EQ(fake_led_level, 0);
	CHECK_EQ(fsm_get_state(), FSM_STATE_SLEEP);
}

/* A reset must NOT open SETUP — a boot loop would otherwise sit in a 180 s
 * advertising window every time (fsm.h). */
static void test_reset_wake_skips_setup(void)
{
	reset_fakes();
	fake_wake = WAKE_REASON_RESET;
	fsm_run();

	CHECK(at("ble_config_run") < 0);
	CHECK(at("measure_run") >= 0);
	CHECK(at("power_off") >= 0);
}

/* --- transmission outcomes -------------------------------------------- */

static void test_tx_success_updates_baseline(void)
{
	reset_fakes();
	fake_anomaly = (anomaly_result_t){ .should_tx = true, .flags = ANOMALY_FLAG_WEIGHT };
	fake_tx_pending = 1;
	fsm_run();

	CHECK(at("transmit_run") > at("lora_init"));
	CHECK(at("lora_session_save") > at("transmit_run"));
	CHECK_EQ(fake_transmit_flags, ANOMALY_FLAG_WEIGHT);
	CHECK(fake_transmit_ext == NULL);  /* Extended Mode off */
	CHECK(fake_last_w_set && fake_last_w == 42000);
	CHECK(fake_last_t_set && fake_last_t == 2150);
	CHECK_EQ(fake_tx_pending, 0);
	CHECK_EQ(fsm_get_state(), FSM_STATE_SLEEP);
}

/* An invalid reading's sentinel must never become the delta baseline. */
static void test_tx_success_keeps_baseline_of_invalid_fields(void)
{
	reset_fakes();
	fake_anomaly = (anomaly_result_t){ .should_tx = true, .flags = ANOMALY_FLAG_HEARTBEAT };
	fake_meas.valid = MEAS_VALID_BATTERY;
	fsm_run();

	CHECK(at("transmit_run") >= 0);
	CHECK(!fake_last_w_set);
	CHECK(!fake_last_t_set);
}

static void test_tx_failure_sets_pending(void)
{
	reset_fakes();
	fake_anomaly = (anomaly_result_t){ .should_tx = true, .flags = ANOMALY_FLAG_HEARTBEAT };
	fake_transmit_ret = -EIO;
	fsm_run();

	CHECK_EQ(fake_tx_pending, 1);
	CHECK(!fake_last_w_set && !fake_last_t_set);
	CHECK(at("lora_session_save") >= 0);
}

/* Deliberately not sent (SF12 fair use): no retry, baseline untouched. */
static void test_tx_skipped_clears_pending_keeps_baseline(void)
{
	reset_fakes();
	fake_anomaly = (anomaly_result_t){ .should_tx = true, .flags = ANOMALY_FLAG_HEARTBEAT };
	fake_tx_pending = 1;
	fake_transmit_ret = LORA_TX_SKIPPED;
	fsm_run();

	CHECK_EQ(fake_tx_pending, 0);
	CHECK(!fake_last_w_set && !fake_last_t_set);
}

static void test_lora_init_failure_defers_tx(void)
{
	reset_fakes();
	fake_anomaly = (anomaly_result_t){ .should_tx = true, .flags = ANOMALY_FLAG_TEMP };
	fake_lora_init_err = -ENODEV;
	fsm_run();

	CHECK(at("transmit_run") < 0);
	CHECK(at("lora_session_save") < 0);
	CHECK_EQ(fake_tx_pending, 1);
	CHECK(at("power_off") >= 0);
}

/* A failed TX last cycle forces one on the next wake, flagged as heartbeat,
 * even when the anomaly check sees nothing. */
static void test_pending_tx_retried_as_heartbeat(void)
{
	reset_fakes();
	fake_tx_pending = 1;
	fsm_run();

	CHECK(at("transmit_run") >= 0);
	CHECK(fake_transmit_flags & ANOMALY_FLAG_HEARTBEAT);
	CHECK_EQ(fake_tx_pending, 0);
}

/* Extended Mode: the BLE scan runs only on a transmitting wake, and strictly
 * before lora_init() so it cannot straddle a join or RX window. */
static void test_ext_scan_precedes_lora(void)
{
	reset_fakes();
	fake_ext_active = true;
	fake_anomaly = (anomaly_result_t){ .should_tx = true, .flags = ANOMALY_FLAG_HEARTBEAT };
	fsm_run();

	CHECK(at("ext_scan_once") >= 0);
	CHECK(at("lora_init") > at("ext_scan_once"));
	CHECK(fake_transmit_ext != NULL);

	reset_fakes();
	fake_ext_active = true;
	fsm_run();
	CHECK(at("ext_scan_once") < 0);  /* measure-only wake: BT stays off */
}

/* --- when System OFF does not happen ---------------------------------- */

/* Debug mode (power_off -> -ENOTSUP) and a failed power_off both fall back to
 * a k_sleep, sliced so the 60 s watchdog keeps being fed. */
static void test_power_off_fallbacks_feed_watchdog(void)
{
	reset_fakes();
	fake_power_off_ret = -ENOTSUP;
	fsm_run();
	CHECK_EQ(stub_uptime_ms, 180 * 1000);
	CHECK(fake_wdt_feeds >= 180 / 30);

	reset_fakes();
	fake_power_off_ret = -EIO;
	fsm_run();
	CHECK_EQ(stub_uptime_ms, 900 * 1000);
	CHECK(fake_wdt_feeds >= 900 / 30);
}

int main(void)
{
	RUN(test_init);
	RUN(test_timer_wake_measure_only);
	RUN(test_button_wake_runs_setup_then_measures);
	RUN(test_reset_wake_skips_setup);
	RUN(test_tx_success_updates_baseline);
	RUN(test_tx_success_keeps_baseline_of_invalid_fields);
	RUN(test_tx_failure_sets_pending);
	RUN(test_tx_skipped_clears_pending_keeps_baseline);
	RUN(test_lora_init_failure_defers_tx);
	RUN(test_pending_tx_retried_as_heartbeat);
	RUN(test_ext_scan_precedes_lora);
	RUN(test_power_off_fallbacks_feed_watchdog);
	return CHECK_SUMMARY();
}
