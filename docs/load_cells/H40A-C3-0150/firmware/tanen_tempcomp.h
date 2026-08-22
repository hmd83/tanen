/*
 * TANEN BASE - load cell temperature compensation
 *
 * Characterised on: Bosche H40A-C3-0150, 22.0 kg static load, 20.5..38.0 C
 *   k   = -17.734 g/K  (95% CI +/- 0.226)
 *   tau = 25 min
 *   residual sigma = 15.7 g   (uncompensated 102.2 g)
 *
 * NOTE: k is a property of the CELL PLUS ITS MOUNT, not of the H40A alone.
 *       Re-characterise after any mechanical change to the frame.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef TANEN_TEMPCOMP_H_
#define TANEN_TEMPCOMP_H_

#include <stdint.h>
#include <stdbool.h>

/* ---- calibration constants -------------------------------------------- */

/* Compensation gain in MILLIGRAM PER KELVIN.
 * k_c = -k = +17.734 g/K = 17734 mg/K. Exact in integer form, no rounding.
 */
#define TANEN_TC_GAIN_MG_PER_K   17734L   /* +17.734 g/K */

/* First-order thermal lag constant of the cell body, in seconds. */
#define TANEN_TC_TAU_S           1500U    /* 25 min */

/* Reference temperature the correction is anchored to, in milli-degC.
 * Set this to the temperature at which the scale was tared.           */
#define TANEN_TC_T_REF_MDEG      25000L   /* 25.000 C */

/* ---- state ------------------------------------------------------------ */

struct tanen_tempcomp {
	int32_t t_eff_mdeg;   /* filtered cell-body temperature [m degC] */
	bool    primed;       /* false until the first sample seeds the filter */
	int32_t gain_mg_per_k;
	uint32_t tau_s;
	int32_t  t_ref_mdeg;
};

/* ---- API -------------------------------------------------------------- */

/**
 * Initialise with the compile-time defaults above.
 */
void tanen_tempcomp_init(struct tanen_tempcomp *tc);

/**
 * Override the calibration at runtime (e.g. from settings/NVS).
 * @param gain_mg_per_k  compensation gain, milligram per kelvin
 * @param tau_s          thermal time constant, seconds
 * @param t_ref_mdeg     reference temperature, milli-degC
 */
void tanen_tempcomp_configure(struct tanen_tempcomp *tc,
			      int32_t gain_mg_per_k,
			      uint32_t tau_s,
			      int32_t t_ref_mdeg);

/**
 * Reset the lag filter. Call after a long power-down so the filter
 * re-seeds from the current temperature instead of ramping.
 */
void tanen_tempcomp_reset(struct tanen_tempcomp *tc);

/**
 * Apply the correction to one sample.
 *
 * Safe with irregular wake intervals: dt_s is supplied per call, so the
 * filter stays correct across a variable Zephyr deep-sleep duty cycle.
 * The first call seeds t_eff with t_mdeg and applies the correction
 * immediately (no warm-up transient).
 *
 * @param tc       state
 * @param w_raw_mg raw weight, milligram
 * @param t_mdeg   measured temperature, milli-degC
 * @param dt_s     seconds since the previous call (ignored on first call)
 * @return         corrected weight, milligram
 */
int32_t tanen_tempcomp_apply(struct tanen_tempcomp *tc,
			     int32_t w_raw_mg,
			     int32_t t_mdeg,
			     uint32_t dt_s);

/**
 * Current filtered cell-body temperature [m degC]. Log this alongside the
 * weight - it is what the correction actually used.
 */
static inline int32_t tanen_tempcomp_t_eff(const struct tanen_tempcomp *tc)
{
	return tc->t_eff_mdeg;
}

#endif /* TANEN_TEMPCOMP_H_ */
