cmake_minimum_required(VERSION 3.16)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bird_remote)

# The Waveshare BSP defaults to software rotation, which allocates a third
# draw buffer and rotates every 368x20 partial update in memory. On this board
# that path has intermittently stopped flushing after Wi-Fi starts. CO5300
# supports MADCTL hardware rotation, and LVGL transforms touch coordinates, so
# select the smaller and deterministic hardware path. Patch after Component
# Manager resolves the BSP and before its source is compiled.
set(WAVESHARE_BSP_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/managed_components/waveshare__esp32_s3_touch_amoled_1_8/esp32_s3_touch_amoled_1_8.c")
if(EXISTS "${WAVESHARE_BSP_SOURCE}")
    file(READ "${WAVESHARE_BSP_SOURCE}" WAVESHARE_BSP_CONTENT)
    string(FIND "${WAVESHARE_BSP_CONTENT}" ".sw_rotate = true," WAVESHARE_SW_ROTATE_POSITION)
    if(NOT WAVESHARE_SW_ROTATE_POSITION EQUAL -1)
        string(REPLACE ".sw_rotate = true," ".sw_rotate = false,"
               WAVESHARE_BSP_CONTENT "${WAVESHARE_BSP_CONTENT}")
        file(WRITE "${WAVESHARE_BSP_SOURCE}" "${WAVESHARE_BSP_CONTENT}")
        message(STATUS "Configured Waveshare BSP for CO5300 hardware rotation")
    endif()
endif()
