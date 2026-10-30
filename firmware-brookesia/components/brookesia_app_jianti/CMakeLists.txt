set(PROJ_SRC ${CMAKE_CURRENT_LIST_DIR})

file(GLOB_RECURSE PROJ_SRCS_C ${PROJ_SRC}/*.c)
file(GLOB_RECURSE PROJ_SRCS_CPP ${PROJ_SRC}/*.cpp)

# GLOB 结果被缓存，新字体文件显式加入
list(APPEND PROJ_SRCS_C ${PROJ_SRC}/font_jt_b_24_2.c)
list(APPEND PROJ_SRCS_C ${PROJ_SRC}/font_jt_b_32_4.c)
list(APPEND PROJ_SRCS_C ${PROJ_SRC}/font_jt_b_36_4.c)
list(APPEND PROJ_SRCS_C ${PROJ_SRC}/font_jt_b_44_4.c)
list(APPEND PROJ_SRCS_C ${PROJ_SRC}/font_jt_b_20_4.c)

# 旧的 20px 字库 / 按钮小字库 / 烘焙文字图全部改文本渲染，不再编译进固件
list(FILTER PROJ_SRCS_C EXCLUDE REGEX "/(font_jt_cn_20_1|font_jianti_btn30|font_jianti_btn38|font_jianti_btn76|img_jianti_btns)\\.c$")

idf_component_register(
    SRCS  ${PROJ_SRCS_C} ${PROJ_SRCS_CPP}
    INCLUDE_DIRS ${PROJ_SRC}
    REQUIRES brookesia_core esp32_p4_wifi6_touch_lcd_4_3 driver esp_driver_ppa esp_driver_jpeg esp_hw_support esp_lcd esp_lcd_touch esp_lvgl_adapter esp_video bsp_extra esp_netif esp_wifi esp_wifi_remote esp_hosted lwip nvs_flash esp_http_client esp_http_server esp_timer 78__xiaozhi-fonts
)

set_source_files_properties(
    ${PROJ_SRCS_CPP}
    PROPERTIES
        COMPILE_FLAGS "-Wno-missing-field-initializers"
)
