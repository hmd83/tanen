/* Host stand-in for <zephyr/drivers/gpio.h>: one fake pin, enough for the
 * status LED. The test owns the fake_led_* state. */
#ifndef TANEN_HOST_STUB_GPIO_H
#define TANEN_HOST_STUB_GPIO_H

#include <zephyr/kernel.h>

#define GPIO_OUTPUT_INACTIVE 0

struct gpio_dt_spec {
	int pin;
};

#define DT_ALIAS(alias) 0
#define GPIO_DT_SPEC_GET(node, prop) { .pin = (node) }

extern bool fake_led_ready;
extern int  fake_led_level;
extern int  fake_led_configure_err;

static inline bool gpio_is_ready_dt(const struct gpio_dt_spec *spec)
{
	(void)spec;
	return fake_led_ready;
}

static inline int gpio_pin_configure_dt(const struct gpio_dt_spec *spec, int flags)
{
	(void)spec;
	(void)flags;
	return fake_led_configure_err;
}

static inline int gpio_pin_set_dt(const struct gpio_dt_spec *spec, int value)
{
	(void)spec;
	fake_led_level = value;
	return 0;
}

#endif
