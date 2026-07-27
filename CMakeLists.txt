cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(TanenBase)

# Force LoRaWAN 1.0.3 crypto (TTN device registered as MAC V1.0.3)
add_compile_definitions(USE_LRWAN_1_1_X_CRYPTO=0)

# LoRaMac internal headers for NVM session save/restore
target_include_directories(app PRIVATE
    ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/mac
    ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/mac/region
    ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/system
    ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/boards
    ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/radio
    ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/peripherals
)

target_sources(app PRIVATE
    src/main.c
    src/core/fsm.c
    src/core/power.c
    src/core/watchdog.c
    src/drivers/weight.c
    src/drivers/temp.c
    src/drivers/battery.c
    src/drivers/lora.c
    src/config/config.c
    src/features/measurement/measure.c
    src/features/transmission/transmit.c
    src/features/anomaly/anomaly.c
    src/features/ble_config/ble_svc.c
)
