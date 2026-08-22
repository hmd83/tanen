/*
 * TANEN BASE - load cell temperature compensation
 * Integer only. No floats, no allocation, no libm.
 *
 * SPDX-License-Identifier: MIT
 */

#include "tanen_tempcomp.h"

/*
 * The exact filter coefficient is alpha = 1 - exp(-dt/tau).
 * exp() costs flash and cycles we do not want on a coin-cell node, so the
 * one-pole RC approximation is used instead, in Q16 fixed point:
 *
 *     alpha_q16 = 65536 * dt / (dt + tau)
 *
 * It is exact in the limit dt << tau and stays within a couple of percent
 * up to dt = tau. With tau = 1500 s and a typical TANEN wake interval of
 * 300 s the error is well under the 0.5 K resolution of the temperature
 * sensor. It is also unconditionally stable for any dt, which an exp()
 * based form is not without extra clamping.
 */
#define Q16 16

static int32_t alpha_q16(uint32_t dt_s, uint32_t tau_s)
{
	uint64_t den = (uint64_t)dt_s + (uint64_t)tau_s;

	if (den == 0U) {
		return 1 << Q16; /* degenerate: pass through */
	}

	uint64_t a = ((uint64_t)dt_s << Q16) / den;

	if (a > (1ULL << Q16)) {
		a = 1ULL << Q16;
	}
	return (int32_t)a;
}

void tanen_tempcomp_init(struct tanen_tempcomp *tc)
{
	tanen_tempcomp_configure(tc,
				 TANEN_TC_GAIN_MG_PER_K,
				 TANEN_TC_TAU_S,
				 TANEN_TC_T_REF_MDEG);
}

void tanen_tempcomp_configure(struct tanen_tempcomp *tc,
			      int32_t gain_mg_per_k,
			      uint32_t tau_s,
			      int32_t t_ref_mdeg)
{
	tc->gain_mg_per_k = gain_mg_per_k;
	tc->tau_s         = tau_s;
	tc->t_ref_mdeg    = t_ref_mdeg;
	tc->t_eff_mdeg    = 0;
	tc->primed        = false;
}

void tanen_tempcomp_reset(struct tanen_tempcomp *tc)
{
	tc->t_eff_mdeg = 0;
	tc->primed     = false;
}

int32_t tanen_tempcomp_apply(struct tanen_tempcomp *tc,
			     int32_t w_raw_mg,
			     int32_t t_mdeg,
			     uint32_t dt_s)
{
	/* 1. thermal lag filter -------------------------------------------- */
	if (!tc->primed) {
		/* Seed on cold boot: assume the cell body is already at the
		 * measured temperature rather than ramping from 0 C.       */
		tc->t_eff_mdeg = t_mdeg;
		tc->primed     = true;
	} else {
		int32_t a   = alpha_q16(dt_s, tc->tau_s);
		int64_t err = (int64_t)t_mdeg - (int64_t)tc->t_eff_mdeg;

		tc->t_eff_mdeg += (int32_t)((err * a) >> Q16);
	}

	/* 2. linear correction --------------------------------------------- */
	/* dT is in milli-kelvin, gain is in milligram per kelvin:
	 *     correction[mg] = gain[mg/K] * dT[mK] / 1000
	 *
	 * Worst case 17734 mg/K * 60000 mK ~= 1.06e9 - fits int64 easily.
	 * The int32 return holds up to +/- 2147 kg, far beyond Emax.
	 */
	int64_t d_mk = (int64_t)tc->t_eff_mdeg - (int64_t)tc->t_ref_mdeg;
	int64_t corr = ((int64_t)tc->gain_mg_per_k * d_mk) / 1000LL;

	return (int32_t)((int64_t)w_raw_mg + corr);
}
