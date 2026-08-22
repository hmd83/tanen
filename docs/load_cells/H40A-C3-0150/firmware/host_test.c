/*
 * TANEN BASE - host-side replay of a CSV log through the firmware
 * compensation, to verify the fixed-point implementation matches the
 * Python model in ../analysis/tempcomp_analysis.py.
 *
 *   gcc -O2 -std=c99 -o host_test host_test.c tanen_tempcomp.c -lm
 *   ./host_test ../data/Loadcell_B.csv
 *
 * SPDX-License-Identifier: MIT
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tanen_tempcomp.h"

#define SAMPLE_PERIOD_S 256U  /* median cadence of the reference dataset */

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <csv>\n", argv[0]);
		return 1;
	}

	FILE *f = fopen(argv[1], "r");
	if (!f) {
		perror("fopen");
		return 1;
	}

	char line[256];
	if (!fgets(line, sizeof line, f)) {  /* skip header */
		fclose(f);
		return 1;
	}

	struct tanen_tempcomp tc;
	tanen_tempcomp_init(&tc);

	double rs = 0, rs2 = 0, rmin = 1e9, rmax = -1e9;
	double cs = 0, cs2 = 0, cmin = 1e9, cmax = -1e9;
	int n = 0;

	while (fgets(line, sizeof line, f)) {
		char *p = strchr(line, ';');
		if (!p) {
			continue;
		}
		double t = atof(p + 1);
		char *q = strchr(p + 1, ';');
		if (!q) {
			continue;
		}
		double w = atof(q + 1);

		int32_t wc = tanen_tempcomp_apply(&tc, (int32_t)(w * 1e6),
						  (int32_t)(t * 1000),
						  SAMPLE_PERIOD_S);
		double c = wc / 1e6;

		rs += w;  rs2 += w * w;  if (w < rmin) rmin = w;  if (w > rmax) rmax = w;
		cs += c;  cs2 += c * c;  if (c < cmin) cmin = c;  if (c > cmax) cmax = c;
		n++;
	}
	fclose(f);

	if (n == 0) {
		fprintf(stderr, "no samples parsed\n");
		return 1;
	}

	double rm = rs / n, rsd = sqrt(rs2 / n - rm * rm);
	double cm = cs / n, csd = sqrt(cs2 / n - cm * cm);

	printf("n=%d\n", n);
	printf("raw : mean=%.4f kg sigma=%.1f g p2p=%.0f g\n",
	       rm, rsd * 1000, (rmax - rmin) * 1000);
	printf("comp: mean=%.4f kg sigma=%.1f g p2p=%.0f g\n",
	       cm, csd * 1000, (cmax - cmin) * 1000);
	return 0;
}
