#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/logging/log.h>

#include "battery.h"

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

#if DT_NODE_HAS_STATUS(DT_NODELABEL(pmic_charger), okay)

/* On-board nPM1300 charger on bit-banged pmic_i2c (SDA P1.18 / SCL P1.17,
 * pins fixed in the app overlay). VBAT ADC is factory-trimmed — no divider,
 * no calibration factor. */
static const struct device *const charger =
	DEVICE_DT_GET(DT_NODELABEL(pmic_charger));

int battery_init(void)
{
	if (!device_is_ready(charger)) {
		LOG_ERR("nPM1300 charger not ready");
		return -ENODEV;
	}

	LOG_INF("Battery driver initialized (nPM1300)");
	return 0;
}

int battery_read(int32_t *mv)
{
	struct sensor_value val;
	int ret;

	/* Triggers an on-demand VBAT/NTC conversion on the PMIC */
	ret = sensor_sample_fetch(charger);
	if (ret) {
		LOG_ERR("charger fetch failed: %d", ret);
		return ret;
	}

	ret = sensor_channel_get(charger, SENSOR_CHAN_GAUGE_VOLTAGE, &val);
	if (ret) {
		LOG_ERR("VBAT read failed: %d", ret);
		return ret;
	}
	*mv = val.val1 * 1000 + val.val2 / 1000;

	/* Diagnostics: charger + VBUS state */
	struct sensor_value chg = {0}, vbus = {0};
	(void)sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &chg);
	(void)sensor_channel_get(charger, SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS, &vbus);
	LOG_INF("Battery: %d mV (chg=0x%02x vbus=%d)", *mv, chg.val1, vbus.val1);
	return 0;
}

#else /* pmic_charger disabled — sleep-current investigation, see overlay */

int battery_init(void)
{
	LOG_INF("Battery driver disabled (nPM1300 charger off for sleep current)");
	return 0;
}

/* -ENODEV keeps measure_run's failure accounting honest: MEAS_VALID_BATTERY
 * stays clear, transmit.c emits the 0xFFFF sentinel, TTN decodes null. */
int battery_read(int32_t *mv)
{
	ARG_UNUSED(mv);
	return -ENODEV;
}

#endif
