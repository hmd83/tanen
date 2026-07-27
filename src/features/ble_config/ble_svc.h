#ifndef TANENBASE_BLE_SVC_H
#define TANENBASE_BLE_SVC_H

#include <stdint.h>

/**
 * Start BLE GATT config service. Blocks until "done" command or timeout.
 * Enables advertising, handles connections, serves characteristics.
 * On exit: disconnects, stops advertising, disables BLE (strict HFCLK cleanup).
 *
 * @param timeout_s Max seconds to stay in config mode
 * @return 0 on normal exit, negative errno on failure
 */
int ble_config_run(uint32_t timeout_s);

#endif
