cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(TanenBaseTestMeasureLoop)

set(MAIN_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

target_include_directories(app PRIVATE ${MAIN_SRC})

if(CONFIG_LORA)
    target_include_directories(app PRIVATE
        ${ZEPHYR_LORAMAC_NODE_MODULE_DIR}/src/radio
    )
endif()

target_sources(app PRIVATE
    src/main.c
    ${MAIN_SRC}/core/power.c
    ${MAIN_SRC}/config/config.c
    ${MAIN_SRC}/drivers/weight.c
    ${MAIN_SRC}/drivers/temp.c
    ${MAIN_SRC}/drivers/battery.c
    ${MAIN_SRC}/features/measurement/measure.c
)
