include_guard(GLOBAL)

if(NOT IDF_TARGET STREQUAL "esp32c6")
    message(FATAL_ERROR "Switch 2 BLE controller patch supports only ESP32-C6")
endif()

idf_build_get_property(_switch2_idf_path IDF_PATH)
idf_build_get_property(_switch2_python PYTHON)
idf_build_get_property(_switch2_elf_target EXECUTABLE)

set(_switch2_source_archive
    "${_switch2_idf_path}/components/bt/controller/lib_esp32c6/esp32c6-bt-lib/esp32c6/libble_app.a")
set(_switch2_output_archive "${CMAKE_BINARY_DIR}/switch2_ble/libble_app.a")
set(_switch2_preparer "${CMAKE_CURRENT_LIST_DIR}/../tools/prepare_switch2_ble_lib.py")

if(NOT EXISTS "${_switch2_source_archive}")
    message(FATAL_ERROR "Verified ESP32-C6 BLE controller archive is missing: ${_switch2_source_archive}")
endif()
if(NOT EXISTS "${_switch2_preparer}")
    message(FATAL_ERROR "Switch 2 BLE controller preparer is missing: ${_switch2_preparer}")
endif()
if(NOT _switch2_python)
    message(FATAL_ERROR "ESP-IDF did not provide its Python interpreter")
endif()
if(NOT TARGET libble_app)
    message(FATAL_ERROR "ESP-IDF did not create the required libble_app imported target")
endif()
if(NOT _switch2_elf_target OR NOT TARGET "${_switch2_elf_target}")
    message(FATAL_ERROR "ESP-IDF did not create the application ELF target")
endif()

# NimBLE otherwise queues ATT Prepare/Execute Writes and presents the completed
# value as an ordinary offset-zero access callback. Disable that host feature so
# PlayGate prepared writes are rejected before application code can accept them.
add_compile_definitions(MYNEWT_VAL_BLE_ATT_SVR_QUEUED_WRITE=0)

get_target_property(_switch2_imported_archive libble_app IMPORTED_LOCATION)
if(NOT _switch2_imported_archive)
    message(FATAL_ERROR "ESP-IDF libble_app target has no IMPORTED_LOCATION")
endif()
file(REAL_PATH "${_switch2_source_archive}" _switch2_source_archive_real)
file(REAL_PATH "${_switch2_imported_archive}" _switch2_imported_archive_real)
if(NOT "${_switch2_imported_archive_real}" STREQUAL "${_switch2_source_archive_real}")
    message(FATAL_ERROR
        "ESP-IDF libble_app target does not reference the verified archive.\n"
        "Expected: ${_switch2_source_archive_real}\n"
        "Actual:   ${_switch2_imported_archive_real}")
endif()

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_switch2_source_archive}"
    "${_switch2_preparer}")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SWITCH2_AR=${CMAKE_AR}"
        "SWITCH2_OBJDUMP=${CMAKE_OBJDUMP}"
        "${_switch2_python}" "${_switch2_preparer}"
        --idf-path "${_switch2_idf_path}"
        --target esp32c6
        --output "${_switch2_output_archive}"
    RESULT_VARIABLE _switch2_prepare_result
    OUTPUT_VARIABLE _switch2_prepare_stdout
    ERROR_VARIABLE _switch2_prepare_stderr
)
if(NOT _switch2_prepare_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to prepare the verified build-local Switch 2 BLE controller archive "
        "(exit ${_switch2_prepare_result}).\n"
        "${_switch2_prepare_stdout}${_switch2_prepare_stderr}")
endif()

set_property(TARGET libble_app PROPERTY IMPORTED_LOCATION "${_switch2_output_archive}")

# Unit firmware does not start the radio, so no ordinary reference would pull
# this controller object from the archive. Force the verified symbol into every
# final ELF; production already uses it and the explicit reference keeps the
# post-link hard gate meaningful for both projects.
target_link_options("${_switch2_elf_target}" PRIVATE
    "-Wl,--undefined=r_ble_ll_conn_slave_start")

add_custom_command(
    TARGET "${_switch2_elf_target}"
    POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E env
        "SWITCH2_OBJDUMP=${CMAKE_OBJDUMP}"
        "${_switch2_python}" "${_switch2_preparer}"
        --idf-path "${_switch2_idf_path}"
        --target esp32c6
        --verify-elf "$<TARGET_FILE:${_switch2_elf_target}>"
    COMMENT "Verifying the linked Switch 2 BLE controller interval patch"
    VERBATIM
)
