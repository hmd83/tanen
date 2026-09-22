#ifndef TANENBASE_TEMPCOMP_H
#define TANENBASE_TEMPCOMP_H

#include <stdint.h>

/** Load persisted filter state, tare reference temperature and the active
 *  load-cell profile. Call once per wake, before the first
 *  tempcomp_correction_mg(). */
void tempcomp_init(void);

/** Re-read the load-cell profile after the setup page changed it, so the BLE
 *  live view shows the new correction within the same session. The filter
 *  state survives: t_eff is a temperature, and the tare stays valid across a
 *  profile change because the correction is zero at T_ref for any gain. */
void tempcomp_reload(void);

/** Advance the thermal-lag filter with one temperature sample and return the
 *  correction to ADD to the raw weight, in milligrams. dt is derived
 *  internally (sleep interval for the first sample of a wake, measured uptime
 *  delta for later ones — the BLE live view samples every 2 s). */
int32_t tempcomp_correction_mg(int32_t t_mdeg);

/** Filtered cell-body temperature [m°C] behind the last correction. */
int32_t tempcomp_t_eff_mdeg(void);

/** Re-anchor the correction on the current temperature and persist it.
 *  MUST be called by the tare command, together with the zero offset: the
 *  offset and T_ref are two halves of the same zero, and a tare that moves one
 *  without the other leaves a standing k_c * dT error (~89 g at 30 °C against
 *  the 25 °C default). Seeds the filter from the probe if it has no value yet. */
int tempcomp_tare(void);

/** Persist filter state. Call once per cycle, before System OFF. */
int tempcomp_save(void);

#endif
