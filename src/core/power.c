#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/poweroff.h>
#include <zephyr/drivers/timer/nrf_grtc_timer.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/pm/device.h>
#include <zephyr/drivers/mfd/npm13xx.h>
#include <hal/nrf_gpio.h>

#include "power.h"

LOG_MODULE_REGISTER(power, LOG_LEVEL_INF);

/* TanenButton on P0.09 (board DTS: button0 / sw0) */
#define BUTTON_PIN NRF_GPIO_PIN_MAP(0, 9)

/* External TanenButton on D2 = P1.30, SHARED with the SX1262 NRESET net
 * (reset-gpios on the lora0 node). Same behaviour as the on-board one: either
 * press wakes into SETUP, so the two latches are simply OR-ed.
 *
 * Moved here from D19 = P0.00 on 2026-09-07: D2 is on a 7-pin header and the
 * carrier already routes it, where D19 is a XIAO bottom pad that needed a
 * flying wire. Wiring is a bare momentary switch to GND — no external pull-up,
 * no cap. Datasheet 'Port capabilities': P0, P1 and P3 are all wakeup sources
 * with SENSE/DETECT; only P2 is not.
 *
 * Sharing NRESET is safe because SETUP is a once-a-year calibration action and
 * the device is asleep 99.8% of the time (185 s awake/day at MS=900 s /
 * TX=14400 s):
 *  - Asleep: input + pull-up + SENSE_LOW. The pull-up (12/14/16 kOhm) holds
 *    NRESET high — exactly what the old 'drive RST HIGH' did — so the radio
 *    stays in its warm-start sleep.
 *  - A press pulls NRESET low: the SoC wakes AND the SX1262 resets. The radio
 *    reset is free: every wake is a full SoC reset, and sx126x_variant_init()
 *    configures reset-gpios GPIO_OUTPUT_ACTIVE (active LOW), so it drives
 *    NRESET low at device init anyway and holds the radio in reset until the
 *    app runs the LoRa init.
 *  - Awake: the pin belongs to the sx126x driver as an output, so a press is
 *    NOT latched and is simply lost (~0.2% of presses). Shorting a
 *    standard-drive pad to GND is harmless (I_OH,SD 1/3/4 mA vs a 15 mA
 *    all-GPIO budget). The SETUP LED is the user's feedback: no LED, press
 *    again.
 *  - A switch that fails CLOSED holds NRESET low: wake loop draining the cell
 *    plus a radio stuck in reset. Visible server-side within one heartbeat.
 *
 * History: was D11 = P3.00, moved to D19 = P0.00 on 2026-08-29 while chasing a
 * 54 uA System OFF reading blamed on P3 being a peripheral-domain port. THAT
 * WAS WRONG — the 54 uA was a carrier-board leak and a carrier swap fixed it.
 * No port has ever been shown to cost anything; do not cite a power argument
 * for the port choice. Avoid D22/D23 (P0.03/P0.04 carry GRTC PWM/CLKOUT32K). */
#define EXT_BUTTON_PIN NRF_GPIO_PIN_MAP(1, 30)

/* On-board py25q64ha NOR on spi00 — pins per the board dtsi (spi00 pinctrl +
 * cs/hold/wp-gpios). spi00 and the flash node are both disabled, so the
 * jedec,spi-nor driver never runs and never issues DPD: left alone the part
 * sits in standby (~10 uA class) with CS#/HOLD#/WP# floating. Seeed's own
 * low-power guide treats DPD + these exact pin states as mandatory to reach
 * the 3.74 uA System OFF reference for this board. */
#define FLASH_HOLD_PIN NRF_GPIO_PIN_MAP(2, 0)
#define FLASH_SCK_PIN  NRF_GPIO_PIN_MAP(2, 1)
#define FLASH_MOSI_PIN NRF_GPIO_PIN_MAP(2, 2)
#define FLASH_WP_PIN   NRF_GPIO_PIN_MAP(2, 3)
#define FLASH_MISO_PIN NRF_GPIO_PIN_MAP(2, 4)
#define FLASH_CS_PIN   NRF_GPIO_PIN_MAP(2, 5)

#define FLASH_CMD_DPD  0xB9

/* nRF54LM20A anomaly [37] — POWER: "Current consumption might increase after
 * pin reset or power cycle", symptom being a System OFF current higher than
 * spec when System OFF is entered too soon after a pin reset or power cycle.
 * Present in Engineering B and Revision 1. Nordic's workaround is to write 1 to
 * this undocumented POWER register and let >= 40 CPU cycles run before entering
 * System OFF. */
#define ANOMALY_37_REG 0x5005340CUL

/* nPM1300 register map (base, offset) — mirrors the private defines in
 * zephyr/drivers/sensor/nordic/npm13xx_charger/npm13xx_charger.c, which the
 * public mfd API deliberately does not export. */
#define NPM_ADC_BASE          0x05U
#define NPM_ADC_TASK_AUTO     0x0CU  /* periodic NTC/die-temp conversions */
#define NPM_ADC_IBAT_EN       0x24U  /* continuous battery-current sense */

/* Cache: System OFF clears RAM so reason_valid starts false on every boot */
static wake_reason_t cached_reason;
static bool reason_valid;

wake_reason_t power_get_wake_reason(void)
{
    if (reason_valid) {
        return cached_reason;
    }

    uint32_t cause = 0;
    (void)hwinfo_get_reset_cause(&cause);
    hwinfo_clear_reset_cause();

    /* Field diagnostics — a supervised recovery must be visible in logs */
    if (cause & RESET_WATCHDOG) {
        LOG_WRN("*** Recovered from WATCHDOG reset ***");
    }
    if (cause & RESET_CPU_LOCKUP) {
        LOG_WRN("*** Recovered from CPU LOCKUP ***");
    }
    if (cause & RESET_SOFTWARE) {
        LOG_WRN("*** Recovered from fatal-error reboot ***");
    }

    /* Latch can be stale (spurious set when SENSE is re-armed before pullup
     * settles) — always clear so it reflects only this wake. Both buttons mean
     * the same thing, so the two latches are OR-ed; they are read separately
     * only so a field log says which one was pressed. */
    bool latch = nrf_gpio_pin_latch_get(BUTTON_PIN);
    bool ext_latch = nrf_gpio_pin_latch_get(EXT_BUTTON_PIN);
    nrf_gpio_pin_latch_clear(BUTTON_PIN);
    nrf_gpio_pin_latch_clear(EXT_BUTTON_PIN);

    /* GRTC (RESET_CLOCK) = timer wake — authoritative over latch. */
    if (cause & RESET_CLOCK) {
        LOG_INF("Wake: timer (GRTC)");
        cached_reason = WAKE_REASON_TIMER;
    } else if (latch || ext_latch || (cause & RESET_LOW_POWER_WAKE)) {
        LOG_INF("Wake: button%s%s", latch ? " [P0.09]" : "",
                ext_latch ? " [D2]" : "");
        cached_reason = WAKE_REASON_BUTTON;
    } else {
        LOG_INF("Wake: reset (cause=0x%08x)", cause);
        cached_reason = WAKE_REASON_RESET;
    }

    reason_valid = true;
    return cached_reason;
}

/* Suspend SPI bus that drives SX1262 */
static void peripherals_suspend(void)
{
    const struct device *spi = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(spi23));
    if (spi != NULL) {
        pm_device_action_run(spi, PM_DEVICE_ACTION_SUSPEND);
    }
}

/* Quiesce the nPM1300's ADC before System OFF.
 *
 * npm13xx_charger_init() unconditionally sets IBAT_EN=1 (continuous battery-
 * current sense) and TASK_AUTO=1 (periodic NTC/die-temp conversions). The PMIC
 * runs off VBAT, so both keep converting for the entire sleep — this is a
 * documented multi-uA to sub-mA leak (Nordic DevZone reports nPM1300 ship-mode
 * current of 145 uA vs 0.5 uA traced purely to auto-ADC being left armed).
 *
 * Not restored on wake: every wake is a full reset, so the driver's init runs
 * again and re-arms both before the next battery_read(). */
static void pmic_enter_sleep(void)
{
    const struct device *pmic = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(pmic));

    if (pmic == NULL || !device_is_ready(pmic)) {
        return;
    }

    (void)mfd_npm13xx_reg_write(pmic, NPM_ADC_BASE, NPM_ADC_TASK_AUTO, 0U);
    (void)mfd_npm13xx_reg_write(pmic, NPM_ADC_BASE, NPM_ADC_IBAT_EN, 0U);

    /* Let any conversion already in flight retire before the bus goes away */
    k_msleep(5);
}

/* Bit-bang 0xB9 (Deep Power-Down) to the on-board NOR, then park its pins in
 * the low-leakage state Seeed specifies. SPI mode 0, MSB first.
 *
 * Bit-banged rather than enabling spi00 + CONFIG_SPI_NOR: the driver would
 * claim P2.01/02/04/05 on every boot and do a JEDEC-ID probe (which wakes the
 * part back out of DPD) just so we could suspend it again. One command, no
 * driver, and spi00 stays disabled.
 *
 * DPD persists until CS# toggles, so the part stays down for the whole sleep.
 * A wake resets the SoC and floats these pins again, which may release DPD —
 * harmless, we redo this before the next System OFF. */
static void flash_enter_dpd(void)
{
    /* HOLD#/WP# deasserted HIGH before CS# goes active */
    nrf_gpio_cfg_output(FLASH_HOLD_PIN);
    nrf_gpio_pin_set(FLASH_HOLD_PIN);
    nrf_gpio_cfg_output(FLASH_WP_PIN);
    nrf_gpio_pin_set(FLASH_WP_PIN);

    nrf_gpio_cfg_output(FLASH_CS_PIN);
    nrf_gpio_pin_set(FLASH_CS_PIN);
    nrf_gpio_cfg_output(FLASH_SCK_PIN);
    nrf_gpio_pin_clear(FLASH_SCK_PIN);   /* mode 0: clock idles LOW */
    nrf_gpio_cfg_output(FLASH_MOSI_PIN);
    nrf_gpio_pin_clear(FLASH_MOSI_PIN);
    nrf_gpio_cfg_input(FLASH_MISO_PIN, NRF_GPIO_PIN_PULLDOWN);

    k_busy_wait(1);                      /* CS# setup */

    nrf_gpio_pin_clear(FLASH_CS_PIN);
    for (int bit = 7; bit >= 0; bit--) {
        if ((FLASH_CMD_DPD >> bit) & 1) {
            nrf_gpio_pin_set(FLASH_MOSI_PIN);
        } else {
            nrf_gpio_pin_clear(FLASH_MOSI_PIN);
        }
        k_busy_wait(1);                  /* data setup before rising edge */
        nrf_gpio_pin_set(FLASH_SCK_PIN); /* part samples MOSI here */
        k_busy_wait(1);
        nrf_gpio_pin_clear(FLASH_SCK_PIN);
    }
    nrf_gpio_pin_set(FLASH_CS_PIN);      /* CS# rising edge latches the command */

    nrf_gpio_pin_clear(FLASH_MOSI_PIN);  /* park LOW per Seeed's table */
    k_busy_wait(10);                     /* t-enter-dpd = 3 us (board dtsi) */
}

/* Arm a button pin as a System OFF wake source: input, pull-up, sense-LOW.
 * Small settling delay so the pullup pulls the line HIGH before SENSE is armed
 * (else SENSE_LOW latches immediately on a still-LOW line). Latch is cleared
 * after arming to discard any glitch — only a real press during sleep sets it
 * again.
 *
 * A button HELD across this call leaves DETECT high, and the SoC then wakes
 * straight back out of System OFF (datasheet: entering System OFF with DETECT
 * high causes a wakeup reset). Harmless for a human press; a stuck or shorted
 * button loops wake->SETUP->sleep and drains the primary cell. */
static void button_arm_sense(uint32_t pin)
{
    nrf_gpio_cfg_input(pin, NRF_GPIO_PIN_PULLUP);
    k_busy_wait(100);
    nrf_gpio_cfg_sense_set(pin, NRF_GPIO_PIN_SENSE_LOW);
    nrf_gpio_pin_latch_clear(pin);
}

/* Disconnect all app GPIO pins to prevent leakage in System OFF.
 * nrf_gpio_cfg_default() sets pin to: input disconnected, no pull.
 * SX1262 is already in LoRaMAC sleep; internal pull-ups on NRESET
 * and NSS keep them HIGH after disconnect → radio stays sleeping. */
static void gpio_disconnect_all(void)
{
    /* SPI23 pins: SCK=P1.04, MISO=P1.05, MOSI=P1.06 (D8/D9/D10) */
    nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 4));
    nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 5));
    nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 6));

    /* SX1262: drive CS HIGH to keep radio sleeping (floating CS wakes it).
     * RST=P1.30 is deliberately NOT driven here — it is EXT_BUTTON_PIN, armed
     * as input + pull-up + SENSE_LOW at the end of this function. The pull-up
     * holds NRESET high, which is what the old drive-HIGH was for. */
    nrf_gpio_cfg_output(NRF_GPIO_PIN_MAP(1, 3));   /* CS=P1.03 HIGH */
    nrf_gpio_pin_set(NRF_GPIO_PIN_MAP(1, 3));
    nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 29));  /* BUSY=P1.29 */
    nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 31));  /* DIO1=P1.31 */

    /* SW I2C (NAU7802): SDA=P1.09, SCL=P1.08 (D7/D6) */
    nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 9));
    nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 8));

    /* 1-Wire (DS18B20): P1.00 (D0) */
    nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 0));

    /* UART20 console: TX=P1.11, RX=P1.10 */
    nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 11));
    nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 10));

    /* RGB LEDs P1.22/23/24 — active-HIGH, disconnected = off */
    nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 22));
    nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 23));
    nrf_gpio_cfg_default(NRF_GPIO_PIN_MAP(1, 24));

    /* P2.00-P2.05 (py25q64 NOR) are driven by flash_enter_dpd() above — the
     * old "leave at reset defaults" rule floated CS#/HOLD#/WP# and left the
     * part in standby, which is where a large slice of the 19.4 uA went.
     *
     * DO NOT TOUCH:
     * P1.17/P1.18 — nPM1300 bit-banged I2C; external pull-ups hold the idle
     *               bus high.
     * P1.12      — power_en, not connected in schematic.
     * P1.07      — D5 = LORA_RF_SW1, the Wio-SX1262 antenna-switch control.
     *               Leave floating: DIO2 switches the path. Never arm SENSE or
     *               a pull here (parks the switch + bills the sleep budget).
     * P0.07/P0.08 (i2c30), P1.13/P1.14 (pdm20) — disabled, untouched. */

    /* TanenButtons: on-board P0.09 (LP domain) and external D2 = P1.30
     * (peripheral domain, shared with SX1262 NRESET). Either press wakes into
     * SETUP. P1.30 is armed here, after the SX1262 pin sweep above. */
    button_arm_sense(BUTTON_PIN);
    button_arm_sense(EXT_BUTTON_PIN);
}

int power_off(uint32_t sleep_seconds)
{
    if (sleep_seconds == 0) {
        LOG_WRN("Sleep interval 0 — skipping System OFF (debug mode)");
        return -ENOTSUP;
    }

    uint64_t wake_us = (uint64_t)sleep_seconds * 1000000ULL;

    LOG_INF("System OFF — wake in %u s", sleep_seconds);

    /* Flush logs while peripherals still active */
    LOG_PANIC();

    /* Suspend SPI bus */
    peripherals_suspend();

    /* Stop the nPM1300's auto-ADC while its bit-banged I2C (P1.17/P1.18) is
     * still up — gpio_disconnect_all() leaves those pins alone, but the bus
     * must be driven while we talk, so do this first. */
    pmic_enter_sleep();

    /* On-board NOR into deep power-down (before the GPIO sweep — it needs
     * P2.0x driven, and gpio_disconnect_all() must not undo that) */
    flash_enter_dpd();

    /* Disconnect / drive all GPIO pins to safe state */
    gpio_disconnect_all();

    /* Anomaly [37] workaround. Must be followed by >= 40 CPU cycles before
     * System OFF — the GRTC prepare and reset-cause clear below cover that many
     * times over, so no explicit delay is needed here. */
    *(volatile uint32_t *)ANOMALY_37_REG = 1;

    /* GRTC wake — set up last, right before poweroff (matches Nordic sample) */
    int ret = z_nrf_grtc_wakeup_prepare(wake_us);
    if (ret) {
        return ret;
    }

    hwinfo_clear_reset_cause();
    sys_poweroff();
    /* never reached */
    CODE_UNREACHABLE;
}
