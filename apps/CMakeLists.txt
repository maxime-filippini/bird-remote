cmake_minimum_required(VERSION 3.16)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.local")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bird_remote)

# esp_lvgl_port 2.6.2 passes height/width and the height stride to LVGL's
# 180-degree partial-buffer rotator. Rectangular 368x20 chunks are therefore
# read with the wrong geometry and the AMOLED remains black. Patch the managed
# dependency after Component Manager resolves it, before compilation. Remove
# this workaround once the dependency contains the corrected call.
set(LVGL_PORT_DISPLAY_SOURCE
    "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c")
if(EXISTS "${LVGL_PORT_DISPLAY_SOURCE}")
    file(READ "${LVGL_PORT_DISPLAY_SOURCE}" LVGL_PORT_DISPLAY_CONTENT)
    set(LVGL_PORT_ROTATE_180_BUG
        "lv_draw_sw_rotate(color_map, disp_ctx->draw_buffs[2], hh, ww, h_stride, h_stride, LV_DISPLAY_ROTATION_180, cf);")
    set(LVGL_PORT_ROTATE_180_FIX
        "lv_draw_sw_rotate(color_map, disp_ctx->draw_buffs[2], ww, hh, w_stride, w_stride, LV_DISPLAY_ROTATION_180, cf);")
    string(FIND "${LVGL_PORT_DISPLAY_CONTENT}" "${LVGL_PORT_ROTATE_180_BUG}" LVGL_PORT_BUG_POSITION)
    if(NOT LVGL_PORT_BUG_POSITION EQUAL -1)
        string(REPLACE "${LVGL_PORT_ROTATE_180_BUG}" "${LVGL_PORT_ROTATE_180_FIX}"
               LVGL_PORT_DISPLAY_CONTENT "${LVGL_PORT_DISPLAY_CONTENT}")
        file(WRITE "${LVGL_PORT_DISPLAY_SOURCE}" "${LVGL_PORT_DISPLAY_CONTENT}")
        message(STATUS "Patched esp_lvgl_port 180-degree partial-buffer rotation")
    endif()
endif()
