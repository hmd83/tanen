/* Host tests for src/features/measurement/tempcomp.c.
 *
 * The unit is #included rather than linked so the static alpha_q16() is
 * reachable, and so every test drives the real wake path — tempcomp_init(),
 * one tempcomp_correction_mg(), tempcomp_save() — with config/power/temp
 * replaced by the fakes below. RAM does not survive System OFF on the target,
 * so a "wake" here only carries state through the fake ZMS record, exactly as
 * on the device.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/features/measurement/tempcomp.c"

#include "check.h"

/* --- fakes ------------------------------------------------------------- */

int64_t stub_uptime_ms;

static tempcomp_state_t fake_zms_state;
static lc_config_t      fake_lc;
static int32_t          fake_tare_mdeg;
static int              fake_tare_writes;
static uint32_t         fake_ms_interval;
static wake_reason_t    fake_wake;
static int32_t          fake_probe_mdeg;
static int              fake_probe_err;

int config_get_tempcomp_state(tempcomp_state_t *st) { *st = fake_zms_state; return 0; }
int config_set_tempcomp_state(const tempcomp_state_t *st) { fake_zms_state = *st; return 0; }
int config_get_lc(lc_config_t *cfg) { *cfg = fake_lc; return 0; }
int config_get_tare_temp(int32_t *val) { *val = fake_tare_mdeg; return 0; }
int config_set_tare_temp(int32_t val) { fake_tare_mdeg = val; fake_tare_writes++; return 0; }
int config_get_ms_interval(uint32_t *val) { *val = fake_ms_interval; return 0; }
wake_reason_t power_get_wake_reason(void) { return fake_wake; }
int temp_read(int32_t *mc) { *mc = fake_probe_mdeg; return fake_probe_err; }

/* Factory-fresh unit on a timer wake: empty ZMS, the 25 C fallback T_ref and
 * the default 300 s interval. */
static void fresh_unit(int32_t gain_mg_per_k, uint32_t tau_s)
{
	memset(&fake_zms_state, 0, sizeof(fake_zms_state));
	memset(&fake_lc, 0, sizeof(fake_lc));
	fake_lc.id = LC_PROFILE_CUSTOM;
	fake_lc.gain_mg_per_k = gain_mg_per_k;
	fake_lc.tau_s = tau_s;
	fake_tare_mdeg = 25000;
	fake_tare_writes = 0;
	fake_ms_interval = 300;
	fake_wake = WAKE_REASON_TIMER;
	fake_probe_mdeg = 0;
	fake_probe_err = 0;
	stub_uptime_ms = 0;
}

/* One measurement wake as fsm.c runs it: init, one sample, save, System OFF
 * (uptime restarts from zero on the next cold boot). */
static int32_t wake_cycle(int32_t t_mdeg)
{
	stub_uptime_ms = 0;
	tempcomp_init();
	int32_t corr = tempcomp_correction_mg(t_mdeg);

	(void)tempcomp_save();
	return corr;
}

/* --- alpha_q16 --------------------------------------------------------- */

static void test_alpha_q16_edges(void)
{
	/* dt = 0 and tau = 0: degenerate, must pass through rather than divide
	 * by zero. */
	CHECK_EQ(alpha_q16(0, 0), 1 << 16);
	/* dt = 0: no time passed, the filter must not move. */
	CHECK_EQ(alpha_q16(0, 1500), 0);
	/* tau = 0: no lag, t_eff follows the probe. */
	CHECK_EQ(alpha_q16(300, 0), 1 << 16);
	CHECK_EQ(alpha_q16(1, 0), 1 << 16);
	/* The documented operating point: 65536 * 300 / 1800, floored. */
	CHECK_EQ(alpha_q16(300, 1500), 10922);
	CHECK_EQ(alpha_q16(1500, 1500), 1 << 15);
	/* dt + tau would overflow uint32 — the sum is taken in 64 bit. */
	CHECK_EQ(alpha_q16(UINT32_MAX, UINT32_MAX), 1 << 15);
	CHECK_EQ(alpha_q16(UINT32_MAX, 0), 1 << 16);
	CHECK_EQ(alpha_q16(1, UINT32_MAX), 0);
}

/* 0 <= alpha <= 1 for every input is what makes the filter unconditionally
 * stable: it can never overshoot the input or oscillate. Monotonic in dt means
 * a longer sleep always pulls t_eff further toward the probe, never less. */
static void test_alpha_q16_bounded_and_monotonic(void)
{
	static const uint32_t taus[] = { 0, 1, 60, 1500, LC_TAU_MAX_S, UINT32_MAX };
	static const uint32_t dts[] = { 0, 1, 2, 59, 60, 300, 900, 1500, 3600,
					86400, 1000000, UINT32_MAX - 1, UINT32_MAX };

	for (size_t i = 0; i < sizeof(taus) / sizeof(taus[0]); i++) {
		int32_t prev = -1;

		for (size_t j = 0; j < sizeof(dts) / sizeof(dts[0]); j++) {
			int32_t a = alpha_q16(dts[j], taus[i]);

			CHECK(a >= 0 && a <= (1 << 16));
			CHECK(a >= prev);
			prev = a;
		}
	}
}

/* --- the filter -------------------------------------------------------- */

/* A step from 20 C to a constant 30 C. Rising, the filter approaches from
 * below and must never cross the input; it stops within one Q16 quantum of it,
 * because (err * a) >> 16 floors a small positive step to 0 — at most
 * 65536 / a mK, 6 mK (0.1 g at the H40A gain) at the operating point. Falling,
 * the same floor rounds every step away from zero, so it lands exactly. */
static void test_filter_converges_to_constant_input(void)
{
	const int32_t a = alpha_q16(300, 1500);
	const int32_t deadband = ((1 << 16) + a - 1) / a;

	fresh_unit(17734, 1500);
	(void)wake_cycle(20000);
	CHECK_EQ(tempcomp_t_eff_mdeg(), 20000);  /* first sample seeds */

	int32_t prev = tempcomp_t_eff_mdeg();

	for (int i = 0; i < 200; i++) {
		(void)wake_cycle(30000);
		CHECK(tempcomp_t_eff_mdeg() >= prev);
		CHECK(tempcomp_t_eff_mdeg() <= 30000);
		prev = tempcomp_t_eff_mdeg();
	}
	CHECK(30000 - tempcomp_t_eff_mdeg() <= deadband);

	/* One tau (5 wakes at 300 s) covers 1 - (5/6)^5 = 60 % of the step —
	 * slower than the exact filter's 63 %, as the RC form is. */
	fresh_unit(17734, 1500);
	(void)wake_cycle(20000);
	for (int i = 0; i < 5; i++) {
		(void)wake_cycle(30000);
	}
	CHECK(tempcomp_t_eff_mdeg() > 25900 && tempcomp_t_eff_mdeg() < 26100);

	fresh_unit(17734, 1500);
	(void)wake_cycle(30000);
	prev = tempcomp_t_eff_mdeg();
	for (int i = 0; i < 200; i++) {
		(void)wake_cycle(20000);
		CHECK(tempcomp_t_eff_mdeg() <= prev);
		CHECK(tempcomp_t_eff_mdeg() >= 20000);
		prev = tempcomp_t_eff_mdeg();
	}
	CHECK_EQ(tempcomp_t_eff_mdeg(), 20000);

	/* Settled at T_ref + 5 K, the correction is gain * 5 K, to the deadband. */
	fresh_unit(17734, 1500);
	(void)wake_cycle(25000);
	int32_t corr = 0;

	for (int i = 0; i < 200; i++) {
		corr = wake_cycle(30000);
	}
	CHECK(corr <= 17734 * 5 && corr >= 17734 * 5 - 17734 * deadband / 1000 - 1);
}

static void test_tau_zero_follows_probe(void)
{
	static const int32_t t[] = { 20000, 31234, -5000, 38000, 38000, 12345 };

	fresh_unit(17734, 0);
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		int32_t corr = wake_cycle(t[i]);

		CHECK_EQ(tempcomp_t_eff_mdeg(), t[i]);
		CHECK_EQ(corr, (int64_t)17734 * (t[i] - 25000) / 1000);
	}
}

/* dt = 0 reaches alpha_q16() through a measurement interval of 0 (the debug
 * "System OFF skipped" mode). The filter then holds across wakes instead of
 * jumping or dividing by zero. Within one wake the uptime gap is rounded and
 * floored at 1 s, so two samples in the same millisecond still advance it. */
static void test_dt_zero_holds_filter(void)
{
	fresh_unit(17734, 1500);
	(void)wake_cycle(20000);
	fake_ms_interval = 0;
	for (int i = 0; i < 10; i++) {
		(void)wake_cycle(30000);
		CHECK_EQ(tempcomp_t_eff_mdeg(), 20000);
	}

	/* Same wake, same uptime: dt floors to 1 s, alpha = 65536 / 1501 = 43. */
	fresh_unit(17734, 1500);
	stub_uptime_ms = 0;
	tempcomp_init();
	(void)tempcomp_correction_mg(20000);
	(void)tempcomp_correction_mg(30000);
	CHECK_EQ(tempcomp_t_eff_mdeg(), 20000 + (int32_t)(((int64_t)10000 * 43) >> 16));
}

/* The alpha clamp is the filter's only saturation: a sleep vastly longer than
 * tau must land on the input, never past it. */
static void test_no_overshoot_when_dt_dwarfs_tau(void)
{
	fresh_unit(17734, 1);
	fake_ms_interval = 86400;
	(void)wake_cycle(-20000);
	(void)wake_cycle(40000);
	CHECK(tempcomp_t_eff_mdeg() <= 40000);
	CHECK(tempcomp_t_eff_mdeg() >= 40000 - 1);
	(void)wake_cycle(-20000);
	CHECK_EQ(tempcomp_t_eff_mdeg(), -20000);
}

/* tempcomp.c does not clamp its output — measure.c's 0..999 kg clamp does that
 * downstream. What it promises instead is that the int64 intermediate never
 * overflows and the int32 result is exact. Check it at the corners: the gain
 * bound the setup page can store, against the DS18B20's whole -55..+125 C span
 * (180 K, wider than the 60 K the source comment budgets for). */
static void test_correction_exact_across_sensor_range(void)
{
	static const int32_t gains[] = { LC_GAIN_MAX_MG_PER_K, -LC_GAIN_MAX_MG_PER_K,
					 17734, 10681, 1, -1 };
	static const int32_t temps[] = { -55000, 125000, 0, 25000, -1, 1 };
	static const int32_t trefs[] = { -55000, 125000, 25000 };

	for (size_t g = 0; g < sizeof(gains) / sizeof(gains[0]); g++) {
		for (size_t r = 0; r < sizeof(trefs) / sizeof(trefs[0]); r++) {
			for (size_t t = 0; t < sizeof(temps) / sizeof(temps[0]); t++) {
				fresh_unit(gains[g], 0);
				fake_tare_mdeg = trefs[r];
				int32_t corr = wake_cycle(temps[t]);
				int64_t want = (int64_t)gains[g] *
					       ((int64_t)temps[t] - trefs[r]) / 1000;

				CHECK_EQ(corr, want);
			}
		}
	}
	/* 200 g/K over 180 K is 36 kg — far inside int32 milligrams. */
	fresh_unit(LC_GAIN_MAX_MG_PER_K, 0);
	fake_tare_mdeg = -55000;
	CHECK_EQ(wake_cycle(125000), 36000000);
}

/* Documented contract: an uncharacterised profile carries gain 0 and the
 * correction is then a no-op — exactly 0 mg, not "small". The filter keeps
 * running so t_eff is still logged. */
static void test_gain_zero_is_exact_noop(void)
{
	static const int32_t t[] = { 25000, -55000, 125000, 0, 31337, -1, 1 };
	static const int32_t trefs[] = { 25000, -55000, 125000, 0 };

	for (size_t r = 0; r < sizeof(trefs) / sizeof(trefs[0]); r++) {
		fresh_unit(0, 1500);
		fake_tare_mdeg = trefs[r];
		for (int pass = 0; pass < 3; pass++) {
			for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
				CHECK_EQ(wake_cycle(t[i]), 0);
			}
		}
	}

	fresh_unit(0, 1500);
	(void)wake_cycle(20000);
	(void)wake_cycle(30000);
	CHECK(tempcomp_t_eff_mdeg() > 20000);
}

/* --- wake and tare semantics ------------------------------------------ */

/* Only a timer wake knows how long it slept. After a reset or a button wake
 * the stored t_eff may be days old and must be dropped, not filtered from. */
static void test_non_timer_wake_reseeds(void)
{
	static const wake_reason_t reasons[] = { WAKE_REASON_RESET, WAKE_REASON_BUTTON };

	for (size_t i = 0; i < 2; i++) {
		fresh_unit(17734, 1500);
		(void)wake_cycle(20000);
		fake_wake = reasons[i];
		(void)wake_cycle(35000);
		CHECK_EQ(tempcomp_t_eff_mdeg(), 35000);
	}

	fresh_unit(17734, 1500);
	(void)wake_cycle(20000);
	(void)wake_cycle(35000);
	CHECK(tempcomp_t_eff_mdeg() > 20000 && tempcomp_t_eff_mdeg() < 35000);
}

/* Tare re-anchors T_ref on t_eff, not on the probe, so the correction is zero
 * at the moment of tare even while the probe is still lagging. */
static void test_tare_anchors_on_t_eff(void)
{
	fresh_unit(17734, 1500);
	(void)wake_cycle(20000);
	(void)wake_cycle(30000);
	int32_t t_eff = tempcomp_t_eff_mdeg();

	CHECK(t_eff != 30000);
	CHECK_EQ(tempcomp_tare(), 0);
	CHECK_EQ(fake_tare_mdeg, t_eff);
	/* Same wake, probe now reading the filtered value: no correction. */
	CHECK_EQ(tempcomp_correction_mg(t_eff), 0);

	/* Unprimed (live view never ran): seeds from the probe. */
	fresh_unit(17734, 1500);
	tempcomp_init();
	fake_probe_mdeg = 21500;
	CHECK_EQ(tempcomp_tare(), 0);
	CHECK_EQ(fake_tare_mdeg, 21500);

	/* Probe failure: error out and leave T_ref alone. */
	fresh_unit(17734, 1500);
	tempcomp_init();
	fake_probe_err = -EIO;
	CHECK_EQ(tempcomp_tare(), -EIO);
	CHECK_EQ(fake_tare_writes, 0);
	CHECK_EQ(fake_tare_mdeg, 25000);
}

/* --- golden replay of the load-cell datasets -------------------------- */

#ifndef GOLDEN_DIR
#define GOLDEN_DIR "golden"
#endif

/* Replay a gen_golden.py file wake by wake. Every row must match the Python
 * reference to the milligram; then the compensated scatter must stay inside
 * the sigma its report documents, at the precision the report prints it. */
static void replay_golden(const char *name, double sigma_bound_g)
{
	char path[512];
	char line[256];
	int32_t gain = 0;
	uint32_t tau = 0;
	int32_t t_ref = 0;
	int n = 0, mismatches = 0;
	double sum = 0.0, sum2 = 0.0;

	snprintf(path, sizeof(path), "%s/%s.csv", GOLDEN_DIR, name);
	FILE *f = fopen(path, "r");

	CHECK(f != NULL);
	if (f == NULL) {
		fprintf(stderr, "cannot open %s\n", path);
		return;
	}

	while (fgets(line, sizeof(line), f)) {
		if (sscanf(line, "# gain_mg_per_k=%d tau_s=%u t_ref_mdeg=%d",
			   &gain, &tau, &t_ref) == 3) {
			fresh_unit(gain, tau);
			fake_tare_mdeg = t_ref;
			continue;
		}

		unsigned long dt;
		long t_mdeg, w_raw, w_want;

		if (sscanf(line, "%lu;%ld;%ld;%ld", &dt, &t_mdeg, &w_raw, &w_want) != 4) {
			continue;  /* comment or column header */
		}

		fake_ms_interval = (uint32_t)dt;
		int64_t w = w_raw + wake_cycle((int32_t)t_mdeg);

		if (w != w_want && mismatches++ == 0) {
			fprintf(stderr, "%s row %d: got %lld mg, reference %ld mg\n",
				name, n + 1, (long long)w, w_want);
		}
		sum += (double)w / 1000.0;
		sum2 += ((double)w / 1000.0) * ((double)w / 1000.0);
		n++;
	}
	fclose(f);

	CHECK(gain != 0 && n > 100);
	CHECK_EQ(mismatches, 0);

	double mean = sum / n;
	double sigma_g = sqrt(sum2 / n - mean * mean);

	printf("     %-11s n=%d sigma=%.2f g (bound %.2f g)\n",
	       name, n, sigma_g, sigma_bound_g);
	CHECK(sigma_g <= sigma_bound_g);
}

/* The case the tempcomp.c header documents: tau = 25 min, 300 s wakes,
 * sigma = 15.7 g on the H40A reference run (printed to 0.1 g, so < 15.75). */
static void test_golden_h40a_300s(void)
{
	replay_golden("h40a_300s", 15.75);
}

/* Same run at its recorded 256 s cadence — the condition the 15.7 g was
 * fitted under. */
static void test_golden_h40a_rowdt(void)
{
	replay_golden("h40a_rowdt", 15.75);
}

/* SBS-PF-150 report: "firmware replay gain=10681 mg/K tau=1500 s: sigma=21.5 g". */
static void test_golden_sbs_pf150(void)
{
	replay_golden("sbs_pf150", 21.55);
}

int main(void)
{
	RUN(test_alpha_q16_edges);
	RUN(test_alpha_q16_bounded_and_monotonic);
	RUN(test_filter_converges_to_constant_input);
	RUN(test_tau_zero_follows_probe);
	RUN(test_dt_zero_holds_filter);
	RUN(test_no_overshoot_when_dt_dwarfs_tau);
	RUN(test_correction_exact_across_sensor_range);
	RUN(test_gain_zero_is_exact_noop);
	RUN(test_non_timer_wake_reseeds);
	RUN(test_tare_anchors_on_t_eff);
	RUN(test_golden_h40a_300s);
	RUN(test_golden_h40a_rowdt);
	RUN(test_golden_sbs_pf150);
	return CHECK_SUMMARY();
}
