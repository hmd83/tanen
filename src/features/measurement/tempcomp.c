/* Load-cell temperature compensation.
 *
 * Uncompensated, the H40A-C3-0150 in this mount reports a 350 g peak-to-peak
 * swing on a constant 22 kg load over a 17.5 K daily cycle. A two-parameter
 * correction — one gain constant plus a first-order thermal lag filter on the
 * temperature — cuts that to sigma = 15.7 g:
 *
 *     T_eff[n] = T_eff[n-1] + alpha * (T[n] - T_eff[n-1])
 *     W_corr   = W_raw + k_c * (T_eff - T_ref)
 *
 * The lag filter matters because the DS18B20 is not bonded to the cell body:
 * plotting weight against instantaneous temperature opens a hysteresis-looking
 * loop that is pure transport lag, and tau = 25 min collapses it.
 *
 * k_c is a property of the CELL PLUS ITS MOUNT, not of the H40A — re-characterise
 * after any mechanical change to the frame, and set the gain to 0 to disable.
 * Both constants come from the load-cell profile the user picked on the setup
 * page (config_get_lc), so one image serves a generic cell, the H40A and a
 * re-characterised frame alike; an uncharacterised profile carries gain 0 and
 * the correction is then a no-op.
 * Full report + the Python/host-replay reference: docs/load_cells/H40A-C3-0150/.
 *
 * Integer only: no float, no libm, no allocation.
 *
 * State model: RAM is wiped on every System OFF, so t_eff is carried in ZMS
 * across cycles — otherwise the filter would re-seed every 5 minutes and the
 * 25 min lag term would never do anything.
 */

#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "tempcomp.h"
#include "../../config/config.h"
#include "../../core/power.h"
#include "../../drivers/temp.h"

LOG_MODULE_REGISTER(tempcomp, LOG_LEVEL_INF);

#define Q16 16

static tempcomp_state_t state;    /* mirrored in ZMS */
static lc_config_t lc;            /* active profile, resolved constants */
static int32_t  t_ref_mdeg;       /* tare temperature */
static int64_t  last_sample_ms;
static bool     have_last_sample; /* false = first sample of this wake */

/* Exact coefficient is alpha = 1 - exp(-dt/tau). exp() costs flash and cycles
 * we do not want on a coin-cell node, so use the one-pole RC approximation in
 * Q16 fixed point:
 *
 *     alpha_q16 = 65536 * dt / (dt + tau)
 *
 * Exact in the limit dt << tau, within a couple of percent up to dt = tau. At
 * tau = 1500 s and the default 300 s wake interval the error is well under the
 * DS18B20's resolution. It is also unconditionally stable for any dt, which the
 * exp() form is not without extra clamping. */
static int32_t alpha_q16(uint32_t dt_s, uint32_t tau_s)
{
	uint64_t den = (uint64_t)dt_s + (uint64_t)tau_s;

	if (den == 0U) {
		return 1 << Q16;  /* degenerate: pass through */
	}

	uint64_t a = ((uint64_t)dt_s << Q16) / den;

	if (a > (1ULL << Q16)) {
		a = 1ULL << Q16;
	}
	return (int32_t)a;
}

/* Seconds since the previous sample. There is no wall clock across System OFF,
 * so the first sample of a wake has to assume the node slept for the configured
 * measurement interval; within a wake (BLE live view) uptime gives the real gap. */
static uint32_t elapsed_s(void)
{
	int64_t now = k_uptime_get();
	uint32_t dt;

	if (!have_last_sample) {
		uint32_t ms_interval = 300;

		config_get_ms_interval(&ms_interval);
		dt = ms_interval;
	} else {
		int64_t delta_ms = now - last_sample_ms;

		dt = (uint32_t)((delta_ms + 500) / 1000);
		if (dt == 0U) {
			dt = 1U;
		}
	}

	last_sample_ms = now;
	have_last_sample = true;
	return dt;
}

void tempcomp_reload(void)
{
	config_get_lc(&lc);
	LOG_INF("tempcomp: profile %u gain=%d mg/K tau=%us",
		lc.id, lc.gain_mg_per_k, lc.tau_s);
}

void tempcomp_init(void)
{
	config_get_tempcomp_state(&state);
	config_get_tare_temp(&t_ref_mdeg);
	config_get_lc(&lc);
	have_last_sample = false;

	/* Only a timer wake has a known gap since the last sample. After a
	 * reset or button wake the node may have been off for days, so a stored
	 * t_eff is worthless — drop it and re-seed from the next sample. */
	if (power_get_wake_reason() != WAKE_REASON_TIMER) {
		state.primed = 0;
	}

	LOG_INF("tempcomp: profile=%u gain=%d mg/K tau=%us T_ref=%d.%03d C t_eff=%s",
		lc.id, lc.gain_mg_per_k, lc.tau_s,
		t_ref_mdeg / 1000, abs(t_ref_mdeg % 1000),
		state.primed ? "restored" : "re-seeding");
}

int32_t tempcomp_correction_mg(int32_t t_mdeg)
{
	uint32_t dt_s = elapsed_s();

	if (!state.primed) {
		/* Assume the cell body is already at the measured temperature
		 * rather than ramping from 0 C. */
		state.t_eff_mdeg = t_mdeg;
		state.primed = 1;
	} else {
		int32_t a = alpha_q16(dt_s, lc.tau_s);
		int64_t err = (int64_t)t_mdeg - (int64_t)state.t_eff_mdeg;

		state.t_eff_mdeg += (int32_t)((err * a) >> Q16);
	}

	/* dT is milli-kelvin and the gain is milligram per kelvin:
	 *     correction[mg] = gain[mg/K] * dT[mK] / 1000
	 * Worst case is the LC_GAIN_MAX_MG_PER_K ceiling, 200000 mg/K * 60000 mK
	 * ~= 1.2e10 — fits int64 easily, and the int32 result holds far more than
	 * the cell's E_max. */
	int64_t d_mk = (int64_t)state.t_eff_mdeg - (int64_t)t_ref_mdeg;
	int64_t corr = ((int64_t)lc.gain_mg_per_k * d_mk) / 1000LL;

	LOG_INF("T=%d.%03d C T_eff=%d.%03d C (dt=%us) corr=%+d mg",
		t_mdeg / 1000, abs(t_mdeg % 1000),
		state.t_eff_mdeg / 1000, abs(state.t_eff_mdeg % 1000),
		dt_s, (int32_t)corr);

	return (int32_t)corr;
}

int32_t tempcomp_t_eff_mdeg(void)
{
	return state.t_eff_mdeg;
}

/* T_ref is anchored to T_eff, not to the instantaneous probe reading: the
 * correction is evaluated against T_eff, so anchoring anywhere else bakes the
 * probe-to-body lag straight into the zero. Writing t_ref_mdeg here as well as
 * through config matters — the cached copy is only reloaded by tempcomp_init()
 * on the next wake, and without this the BLE live view would keep showing the
 * pre-tare correction for the rest of the setup window. */
int tempcomp_tare(void)
{
	if (!state.primed) {
		/* Nothing has driven the filter yet this wake (live view never
		 * subscribed) — seed it from the probe so T_ref is a real
		 * temperature and not a leftover zero. */
		int32_t t_mdeg;
		int err = temp_read(&t_mdeg);

		if (err) {
			LOG_ERR("tare: temp read failed (%d) — T_ref left at %d.%03d C",
				err, t_ref_mdeg / 1000, abs(t_ref_mdeg % 1000));
			return err;
		}
		state.t_eff_mdeg = t_mdeg;
		state.primed = 1;
	}

	t_ref_mdeg = state.t_eff_mdeg;
	LOG_INF("tare: T_ref := %d.%03d C", t_ref_mdeg / 1000, abs(t_ref_mdeg % 1000));
	return config_set_tare_temp(t_ref_mdeg);
}

int tempcomp_save(void)
{
	return config_set_tempcomp_state(&state);
}
