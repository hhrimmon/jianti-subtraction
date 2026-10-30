set(srcs "main.c" "app_video.c" "uplink.c" "prov.c" "dns_server.c")

# v0.3 CJK font: compiled in only when the generated font file is present,
# so the build never breaks if the font step was skipped.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/jt_font_28.c)
    list(APPEND srcs "jt_font_28.c")
    add_compile_definitions(JT_HAS_CJK_FONT=1)
endif()

# Setup-screen huge font: ASCII + 5 prompt glyphs only, tiny file.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/jt_font_big.c)
    list(APPEND srcs "jt_font_big.c")
    add_compile_definitions(JT_HAS_BIG_FONT=1)
endif()

# Setup-screen SSID font: 80px bold, ASCII only (SSID is ASCII).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/jt_font_big_bold.c)
    list(APPEND srcs "jt_font_big_bold.c")
    add_compile_definitions(JT_HAS_BIG_BOLD_FONT=1)
endif()

idf_component_register(SRCS "${srcs}"
                       PRIV_INCLUDE_DIRS .
                       PRIV_REQUIRES esp_driver_jpeg esp_console mbedtls
                                     esp_http_client esp_http_server esp_timer
                                     esp_wifi esp_netif esp_event nvs_flash lwip)
idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    if(TARGET ${LVGL_LIB})
        target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes -Wno-error=cpp)
    endif()
endif()
