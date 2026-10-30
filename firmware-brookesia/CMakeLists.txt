# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-p4-lcd-4-3-brookesia)

if(TARGET __idf_espressif__mcp-c-sdk AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(__idf_espressif__mcp-c-sdk PRIVATE
        -Wno-error=stringop-truncation
    )
endif()

if(TARGET __idf_espressif__esp-boost AND CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    target_compile_options(__idf_espressif__esp-boost PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-Wno-error=deprecated-variadic-comma-omission>
    )
endif()

# esp_video v2.0.1 only defines ESP_VIDEO_CSI_DEVICE_CONV_FORMAT for IDF>=6.0.
# On IDF 5.5.6 this leaves the CSI bridge trying to do RAW8->RGB565 (only ISP can).
# Define it ONLY for the CSI device source so the CSI bypasses conversion and the
# ISP handles RAW->RGB. (Not for isp_device.c: on IDF 5.5.6 the YUV422 pack-order
# enum aliases collide and would produce duplicate-case errors.)
if(TARGET __idf_espressif__esp_video)
    set(_esp_video_csi_dev_src
        "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_video/src/device/esp_video_csi_device.c")
    if(EXISTS "${_esp_video_csi_dev_src}")
        set_source_files_properties("${_esp_video_csi_dev_src}"
            TARGET_DIRECTORY __idf_espressif__esp_video
            PROPERTIES COMPILE_DEFINITIONS "ESP_VIDEO_CSI_DEVICE_CONV_FORMAT=1")
    endif()
    unset(_esp_video_csi_dev_src)
endif()
