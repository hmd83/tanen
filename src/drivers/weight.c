/* NAU7802 self-contained driver — stability-first read.
 * VIN=3.3V (no AVDD), no DRDY pin (CR polled over I2C), sw-I2C @ 100 kHz.
 * 10 SPS gain 128 chopper-OFF (Adafruit ref-lib defaults — lowest noise).
 *
 * Sleep model: weight_sleep clears PUD → chip enters ~1µA power-down.
 *
 * Stability model: with the chopper disabled the PGA/ADC offset is NOT
 * auto-cancelled in hardware and drifts with temperature/supply over hours.
 * So, like Adafruit's calibrate() on every begin(), cold_init runs a fresh
 * internal offset calibration on every wake → baseline tracks the drift.
 * Internal cal shorts the PGA inputs internally: it measures only the AFE
 * offset, never the load-cell signal, so the software tare stays valid.
 * Costs a little more time/power per cycle; buys a stable baseline. */

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>
#include <stdint.h>
#include <string.h>

#include "weight.h"

LOG_MODULE_REGISTER(weight, LOG_LEVEL_INF);

#define NAU7802_ADDR 0x2A

/* ---- Registers ---- */
#define REG_PU_CTRL  0x00
#define REG_CTRL1    0x01
#define REG_CTRL2    0x02
#define REG_OCAL1_B2 0x03
#define REG_OCAL1_B1 0x04
#define REG_OCAL1_B0 0x05
#define REG_ADCO_B2  0x12   /* burst-readable 3 bytes B2..B0 */
#define REG_ADC      0x15
#define REG_PGA      0x1B
#define REG_POWER    0x1C

/* PU_CTRL bits */
#define PU_RR    BIT(0)
#define PU_PUD   BIT(1)
#define PU_PUA   BIT(2)
#define PU_PUR   BIT(3)   /* RO */
#define PU_CS    BIT(4)
#define PU_CR    BIT(5)   /* RO */
#define PU_AVDDS BIT(7)   /* 1 = internal LDO (we use this, VIN=3.3V) */
#define PU_ROMASK (PU_PUR | PU_CR)

/* CTRL1: VLDO[5:3]=3.0V (5), GAIN[2:0]=128 (7) */
#define CTRL1_VAL ((5 << 3) | 7)

/* CTRL2: CRS[6:4]=10 SPS (0) — Adafruit stable default, lowest noise.
 * Slower than 80 SPS (≈100 ms/sample) but far less ADC noise per reading. */
#define CTRL2_CRS_10   (0 << 4)
#define CTRL2_CRS_MASK (BIT(6) | BIT(5) | BIT(4))
#define CTRL2_CALS     BIT(2)
#define CTRL2_CALERR   BIT(3)

/* One conversion per ~100 ms at CRS=10 SPS — the steady-state CR period. */
#define CONV_PERIOD_MS 100

/* First-CR budget after the conversion train is (re)started with CS, or after
 * an offset calibration. The sigma-delta's sinc filter has to refill before it
 * produces a valid result, so the FIRST sample lands ~3 conversion periods out,
 * not one. Measured on the LM20A board: CS asserted right after the OCAL1 log
 * at t=05.5426, no CR by the old 50+200 ms deadline at t=05.7941, and the next
 * three samples completed by t=06.1473 — putting the first CR ~300-350 ms
 * after CS. 200 ms therefore missed it on EVERY cold init.
 *
 * 5 periods gives real margin over the measured 350 ms. It is a ceiling, not a
 * delay: once settled each sample returns in ~100 ms, so the wider window is
 * never actually spent. Only a genuinely stuck chip waits the full budget, and
 * that case breaks out of the flush loop anyway. */
#define CONV_SETTLE_MS (5 * CONV_PERIOD_MS)

/* REG_CHPS bits[5:4]: 00 = chopper enabled (default), 11 = chopper DISABLED.
 * Adafruit's stable reference lib writes 0b11. Chopper-on causes CR pulses
 * to land on internal pre-chopper samples → unstable trios we saw. */
#define ADC_CHOP_OFF  (0x3 << 4)
#define POWER_PGA_CAP BIT(7)

static const struct device *i2c_dev = DEVICE_DT_GET(DT_NODELABEL(sw_i2c));

/* ---- I2C helpers ----
 * Every transfer gets ONE non-blocking recovery attempt: a slave holding
 * SDA low (NAU7802 mid-read when the last cycle browned out / ESD glitch)
 * wedges the bus forever otherwise. i2c_recover_bus() on the gpio-i2c
 * driver clocks out 9 SCL pulses + STOP — microseconds, no reset needed. */

static int i2c_retry(int err)
{
	if (!err) {
		return 0;
	}
	LOG_WRN("I2C err %d — bus recovery", err);
	int rc = i2c_recover_bus(i2c_dev);
	if (rc) {
		LOG_ERR("bus recovery failed: %d", rc);
	}
	return err;
}

static int reg_rd(uint8_t reg, uint8_t *v)
{
	int ret = i2c_reg_read_byte(i2c_dev, NAU7802_ADDR, reg, v);
	if (i2c_retry(ret)) {
		ret = i2c_reg_read_byte(i2c_dev, NAU7802_ADDR, reg, v);
	}
	return ret;
}

static int reg_wr(uint8_t reg, uint8_t v)
{
	int ret = i2c_reg_write_byte(i2c_dev, NAU7802_ADDR, reg, v);
	if (i2c_retry(ret)) {
		ret = i2c_reg_write_byte(i2c_dev, NAU7802_ADDR, reg, v);
	}
	return ret;
}

static int pu_update(uint8_t set, uint8_t clr)
{
	uint8_t v;
	int ret = reg_rd(REG_PU_CTRL, &v);
	if (ret) return ret;
	v = (v | set) & ~clr;
	v &= ~PU_ROMASK;
	return reg_wr(REG_PU_CTRL, v);
}

static int poll_pu_bit(uint8_t bit, uint32_t timeout_ms)
{
	uint32_t end = k_uptime_get_32() + timeout_ms;
	uint8_t v;
	do {
		int ret = reg_rd(REG_PU_CTRL, &v);
		if (ret) return ret;
		if (v & bit) return 0;
		/* k_msleep, NOT k_busy_wait: CR at 10 SPS arrives every ~100 ms;
		 * busy-waiting keeps the CPU at full active current for the whole
		 * conversion train (~2.5 mA × seconds per cycle = real battery). */
		k_msleep(2);
	} while (k_uptime_get_32() < end);
	return -ETIMEDOUT;
}

/* Burst read ADCO 3 bytes (B2 MSB → B0 LSB), sign-extend 24-bit */
static int read_adco(int32_t *out)
{
	uint8_t reg = REG_ADCO_B2;
	uint8_t b[3];
	int ret = i2c_write_read(i2c_dev, NAU7802_ADDR, &reg, 1, b, sizeof(b));
	if (i2c_retry(ret)) {
		ret = i2c_write_read(i2c_dev, NAU7802_ADDR, &reg, 1, b, sizeof(b));
	}
	if (ret) return ret;
	int32_t v = ((int32_t)b[0] << 16) | ((int32_t)b[1] << 8) | b[2];
	if (v & 0x800000) v |= 0xFF000000;
	*out = v;
	return 0;
}

/* ---- Init (cold) ---- */

static int offset_cal(void)
{
	int ret = reg_wr(REG_CTRL2, CTRL2_CRS_10 | CTRL2_CALS);
	if (ret) return ret;

	uint32_t end = k_uptime_get_32() + 500;
	uint8_t c2;
	do {
		ret = reg_rd(REG_CTRL2, &c2);
		if (ret) return ret;
		if (!(c2 & CTRL2_CALS)) break;
		k_msleep(2);
	} while (k_uptime_get_32() < end);

	if (c2 & CTRL2_CALS)   { LOG_ERR("cal timeout"); return -EIO; }
	if (c2 & CTRL2_CALERR) { LOG_ERR("cal error");   return -EIO; }
	return reg_wr(REG_CTRL2, CTRL2_CRS_10);
}

static int cold_init(void)
{
	int ret;

	ret = reg_wr(REG_PU_CTRL, PU_RR);            if (ret) return ret;
	ret = reg_wr(REG_PU_CTRL, PU_PUD);           if (ret) return ret;
	k_usleep(300);

	ret = poll_pu_bit(PU_PUR, 100);
	if (ret) { LOG_ERR("PUR after PUD"); return ret; }

	/* PUA on + internal LDO (AVDDS=1, VIN=3.3V drives LDO) */
	ret = pu_update(PU_PUA | PU_AVDDS, 0);       if (ret) return ret;
	k_msleep(10);
	ret = poll_pu_bit(PU_PUR, 600);
	if (ret) { LOG_ERR("PUR after PUA"); return ret; }

	ret = reg_wr(REG_CTRL1, CTRL1_VAL);          if (ret) return ret;
	ret = reg_wr(REG_CTRL2, CTRL2_CRS_10);      if (ret) return ret;
	ret = reg_wr(REG_ADC, ADC_CHOP_OFF);         if (ret) return ret;

	uint8_t v;
	ret = reg_rd(REG_PGA, &v);                   if (ret) return ret;
	v &= ~BIT(6);                                /* LDOMODE=0 */
	ret = reg_wr(REG_PGA, v);                    if (ret) return ret;

	ret = reg_rd(REG_POWER, &v);                 if (ret) return ret;
	v |= POWER_PGA_CAP;
	ret = reg_wr(REG_POWER, v);                  if (ret) return ret;

	/* Fresh internal offset calibration on EVERY wake (Adafruit calls
	 * calibrate() on every begin() for exactly this reason). With the chopper
	 * off the AFE offset drifts with temp/supply over hours; re-cal each cycle
	 * tracks it → stable baseline. Internal cal shorts the PGA inputs, so it
	 * captures only the AFE offset, never the load-cell signal → software tare
	 * (config zero_offset) stays valid across cycles. */
	ret = offset_cal();
	if (ret) return ret;

	uint8_t b2, b1, b0;
	if (!reg_rd(REG_OCAL1_B2, &b2) && !reg_rd(REG_OCAL1_B1, &b1) && !reg_rd(REG_OCAL1_B0, &b0)) {
		int32_t o = ((int32_t)b2 << 16) | ((int32_t)b1 << 8) | b0;
		if (o & 0x800000) o |= 0xFF000000;
		LOG_INF("OCAL1=%d (fresh internal cal)", o);
	}

	/* Start continuous conversions. Leave PUA on — caller will weight_sleep
	 * before deep sleep. Stability requires NO PUA cycling between reads. */
	ret = pu_update(PU_CS, 0);
	if (ret) return ret;
	k_msleep(50);

	/* Flush 5 samples to clear startup transient. CONV_SETTLE_MS, not one
	 * conversion period: the first sample after CS is still filter-settling. */
	for (int i = 0; i < 5; i++) {
		ret = poll_pu_bit(PU_CR, CONV_SETTLE_MS);
		if (ret) { LOG_WRN("cold flush[%d] timeout", i); break; }
		int32_t dummy;
		(void)read_adco(&dummy);
	}
	return 0;
}

/* ---- Public API ---- */

int weight_init(void)
{
	int ret;

	if (!device_is_ready(i2c_dev)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	/* Brownout guard: PUD set AND CRS marker matches → regs intact.
	 * Warm path still cycles PUA off→on + re-cal to flush stale state. */
	uint8_t pu, c2;
	ret = reg_rd(REG_PU_CTRL, &pu);
	if (ret || !(pu & PU_PUD)) {
		LOG_INF("NAU7802 cold init");
		return cold_init();
	}
	ret = reg_rd(REG_CTRL2, &c2);
	if (ret || (c2 & CTRL2_CRS_MASK) != CTRL2_CRS_10) {
		LOG_INF("NAU7802 cold init (regs stale)");
		return cold_init();
	}

	LOG_INF("NAU7802 warm init");

	/* WARM PATH: regs persisted (chip kept VIN power), but still re-cal the
	 * offset to track drift — internal cal doesn't touch the load signal, so
	 * tare stays valid (same rationale as cold_init). */
	ret = pu_update(PU_PUA, 0);
	if (ret) return ret;
	ret = poll_pu_bit(PU_PUR, 200);
	if (ret) { LOG_ERR("warm PUR"); return ret; }
	k_msleep(50); /* LDO/PGA settle */

	ret = offset_cal();
	if (ret) return ret;

	ret = pu_update(PU_CS, 0);
	if (ret) return ret;
	k_msleep(50);

	/* Flush 5 samples to clear any stale data after PUA cycle. The warm path
	 * failed one iteration later than the cold one (flush[1], not flush[0]):
	 * CR was still latched from before the PUA cycle, so flush[0] consumed
	 * that stale sample instantly and flush[1] was the first to actually wait
	 * on the settling filter. Hence the settle budget applies to every
	 * iteration, not just the first. */
	for (int i = 0; i < 5; i++) {
		ret = poll_pu_bit(PU_CR, CONV_SETTLE_MS);
		if (ret) { LOG_WRN("flush[%d] timeout", i); break; }
		int32_t dummy;
		(void)read_adco(&dummy);
	}

	return 0;
}

int weight_sleep(void)
{
	/* Full power-down: clear PUD → chip drops to ~1µA (vs ~200µA in
	 * PUD-only standby). cold_init re-calibrates on the next wake. */
	return reg_wr(REG_PU_CTRL, 0);
}

/* Read tuning:
 *  - CLUSTER_DELTA: max range across the 3 tightest samples to accept
 *  - CLUSTER_SIZE:  how many samples must agree (in sorted-cluster sense)
 *  - MIN_SAMPLES:   take at least this many before scanning for cluster
 *  - MAX_SAMPLES:   cap before giving up (16 = ~200ms, 24 = ~300ms @ 80SPS)
 *
 * Algorithm: take samples in batches. After each batch, sort + slide a
 * 3-wide window through sorted array to find tightest cluster. If tightest
 * range < CLUSTER_DELTA, return their mean. Otherwise take more samples.
 *
 * Why not "N consecutive in time": bursts of BLE/RF noise scatter outliers
 * between good samples (e.g., `[14162 32815 34882]` — 2 valid + 1 outlier).
 * Sorting clusters the valid ones together regardless of arrival order. */
#define CLUSTER_DELTA 250
#define CLUSTER_SIZE  3
#define MIN_SAMPLES   3
#define MAX_SAMPLES   24

static void sort_i32(int32_t *a, int n)
{
	for (int i = 1; i < n; i++) {
		int32_t k = a[i];
		int j = i - 1;
		while (j >= 0 && a[j] > k) { a[j + 1] = a[j]; j--; }
		a[j + 1] = k;
	}
}

/* Returns range (max-min) of tightest CLUSTER_SIZE window in sorted array,
 * writes mean into *out_mean. */
static int32_t tightest_cluster(const int32_t *sorted, int n, int32_t *out_mean)
{
	int32_t best_range = INT32_MAX;
	int     best_idx   = 0;
	for (int i = 0; i + CLUSTER_SIZE <= n; i++) {
		int32_t r = sorted[i + CLUSTER_SIZE - 1] - sorted[i];
		if (r < best_range) { best_range = r; best_idx = i; }
	}
	int32_t sum = 0;
	for (int i = 0; i < CLUSTER_SIZE; i++) sum += sorted[best_idx + i];
	*out_mean = sum / CLUSTER_SIZE;
	return best_range;
}

int weight_read(int32_t *raw)
{
	int32_t buf[MAX_SAMPLES];
	int32_t sorted[MAX_SAMPLES];
	int ret;

	for (int n = 0; n < MAX_SAMPLES; n++) {
		ret = poll_pu_bit(PU_CR, 200);
		if (ret) { LOG_ERR("CR[%d]", n); return ret; }
		ret = read_adco(&buf[n]);
		if (ret) return ret;

		/* Scan for cluster only after MIN_SAMPLES, then every sample */
		if (n + 1 >= MIN_SAMPLES) {
			int count = n + 1;
			memcpy(sorted, buf, count * sizeof(int32_t));
			sort_i32(sorted, count);

			int32_t mean;
			int32_t range = tightest_cluster(sorted, count, &mean);
			if (range < CLUSTER_DELTA) {
				*raw = mean;
				LOG_INF("NAU7802 raw=%d (n=%d range=%d)",
					*raw, count, range);
				return 0;
			}
		}
	}

	/* No tight cluster found — return tightest-anyway + warn */
	memcpy(sorted, buf, MAX_SAMPLES * sizeof(int32_t));
	sort_i32(sorted, MAX_SAMPLES);
	int32_t mean;
	int32_t range = tightest_cluster(sorted, MAX_SAMPLES, &mean);
	*raw = mean;
	LOG_WRN("NAU7802 raw=%d (UNSTABLE n=%d range=%d)",
		*raw, MAX_SAMPLES, range);
	return 0;
}
