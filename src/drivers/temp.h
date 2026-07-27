#ifndef TANENBASE_TEMP_H
#define TANENBASE_TEMP_H

int temp_init(void);

/* Returns temperature in milli-Celsius (e.g. 23500 = 23.5°C) */
int temp_read(int32_t *mc);

#endif
