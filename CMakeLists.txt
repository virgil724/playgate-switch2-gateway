cmake_minimum_required(VERSION 3.22)

if(DEFINED ENV{IDF_TARGET} AND NOT "$ENV{IDF_TARGET}" STREQUAL "esp32c6")
    message(FATAL_ERROR "PlayGate Switch 2 Gateway requires IDF_TARGET=esp32c6")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(playgate_switch_gateway)

idf_build_get_property(playgate_idf_target IDF_TARGET)
if(NOT playgate_idf_target STREQUAL "esp32c6")
    message(FATAL_ERROR "PlayGate Switch 2 Gateway supports only ESP32-C6")
endif()
if(NOT IDF_VERSION_MAJOR EQUAL 6
   OR NOT IDF_VERSION_MINOR EQUAL 1
   OR NOT IDF_VERSION_PATCH EQUAL 0)
    message(FATAL_ERROR "PlayGate Switch 2 Gateway requires ESP-IDF v6.1.0 exactly")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/cmake/switch2_ble_patch.cmake")
