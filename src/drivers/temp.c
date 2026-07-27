#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "temp.h"

LOG_MODULE_REGISTER(temp, LOG_LEVEL_INF);

static const struct device *ds18b20_dev = DEVICE_DT_GET_ONE(maxim_ds18b20);


int temp_init(void)
{
	if (!device_is_ready(ds18b20_dev)) {
		LOG_ERR("DS18B20 not ready");
		return -ENODEV;
	}
	LOG_INF("DS18B20 initialized");
	return 0;
}

int temp_read(int32_t *mc)
{
	int ret = sensor_sample_fetch(ds18b20_dev);
	if (ret) {
		LOG_ERR("fetch failed: %d", ret);
		return ret;
	}

	struct sensor_value val;
	ret = sensor_channel_get(ds18b20_dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
	if (ret) {
		LOG_ERR("channel_get failed: %d", ret);
		return ret;
	}

	*mc = val.val1 * 1000 + val.val2 / 1000;
	LOG_INF("DS18B20: %d.%03d C", val.val1, val.val2 / 1000);
	return 0;
}
