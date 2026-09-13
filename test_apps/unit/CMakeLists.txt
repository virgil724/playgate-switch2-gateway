cmake_minimum_required(VERSION 3.22)

if(DEFINED ENV{IDF_TARGET} AND NOT "$ENV{IDF_TARGET}" STREQUAL "esp32c6")
    message(FATAL_ERROR "Gateway unit tests require IDF_TARGET=esp32c6")
endif()

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(playgate_gateway_unit_tests)

idf_build_get_property(playgate_test_target IDF_TARGET)
if(NOT playgate_test_target STREQUAL "esp32c6")
    message(FATAL_ERROR "Gateway unit tests support only ESP32-C6")
endif()
if(NOT IDF_VERSION_MAJOR EQUAL 6
   OR NOT IDF_VERSION_MINOR EQUAL 1
   OR NOT IDF_VERSION_PATCH EQUAL 0)
    message(FATAL_ERROR "Gateway unit tests require ESP-IDF v6.1.0 exactly")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/switch2_ble_patch.cmake")
