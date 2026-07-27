#ifndef TANENBASE_BATTERY_H
#define TANENBASE_BATTERY_H

int battery_init(void);

/* Returns battery voltage in millivolts */
int battery_read(int32_t *mv);

#endif
