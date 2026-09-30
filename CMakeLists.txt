cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(TanenBase)

# Force LoRaWAN 1.0.3 crypto (TTN device registered as MAC V1.0.3)
add_compile_definitions(USE_LRWAN_1_1_X_CRYPTO=0)

# LoRaMac's NVM structs change size with REGION_* and SOFT_SE. The library is
# built with them (zephyr/subsys/lorawan/loramac-node, modules/loramac-node);
# lora.c must see the same layout, or sizeof(LoRaMacNvmData_t) comes out short
# (728 B) and the session save silently drops the tail — the join-accept
# CFList channels 3-7 — so every LinkADRReq from TTN was NACKed. lora.c
# BUILD_ASSERTs this.
target_compile_definitions(app PRIVATE $<$<BOOL:${CONFIG_HAS_SEMTECH_SOFT_SE}>:SOFT_SE>)
foreach(region AS923 AU915 CN470 CN779 EU433 EU868 KR920 IN865 US915 RU864)
  if(CONFIG_LORAWAN_REGION_${region})
    target_compile_definitions(app PRIVATE REGION_${region})
  endif()
endforeach()

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
)

# Load-cell temperature compensation is optional: measure.c and fsm.c guard
# every call site with IS_ENABLED(CONFIG_TANENBASE_TEMPCOMP).
target_sources_ifdef(CONFIG_TANENBASE_TEMPCOMP app PRIVATE
    src/features/measurement/tempcomp.c
)

# BLE config service is optional: fsm.c already guards every call site with
# IS_ENABLED(CONFIG_TANENBASE_BLE_CONFIG), so dropping the translation unit is
# enough to build with CONFIG_BT=n (useful for isolating radio-vs-sensor bugs).
target_sources_ifdef(CONFIG_TANENBASE_BLE_CONFIG app PRIVATE
    src/features/ble_config/ble_svc.c
)

# Extended Mode BLE sensors: fsm.c and ble_svc.c guard every call site with
# IS_ENABLED(CONFIG_TANENBASE_EXT_SENSORS); the types in ext_sensors.h stay
# available to transmit.c either way.
target_sources_ifdef(CONFIG_TANENBASE_EXT_SENSORS app PRIVATE
    src/features/ext_sensors/ext_sensors.c
)
