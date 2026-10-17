# SPDX-License-Identifier: Apache-2.0

if(CONFIG_TINYBLE)
  zephyr_include_directories(../include)
  zephyr_library()
  zephyr_library_sources(../src/tinyble.c ../port/zephyr/tb_port_zephyr.c)
  zephyr_library_sources_ifdef(CONFIG_TINYBLE_MATTER_GATT ../src/tb_matter_gatt.c)
  zephyr_library_sources_ifdef(CONFIG_TINYBLE_TINYTALK ../port/tinytalk/matter_ble_tinyble.c)
  # tinyfota's services in the table, and Find By Type Value to find them.
  zephyr_library_compile_definitions_ifdef(CONFIG_TINYBLE_TINYFOTA TB_MATTER_DFU=1 TB_FIND_BY_TYPE=1)
  # The adapter's scan response UUID as 16 bytes, least significant first.
  if(CONFIG_TINYBLE_TINYTALK AND NOT "${CONFIG_TINYBLE_TINYTALK_SCAN_RSP_UUID128}" STREQUAL "")
    string(REPLACE "-" "" tb_uuid "${CONFIG_TINYBLE_TINYTALK_SCAN_RSP_UUID128}")
    string(LENGTH "${tb_uuid}" tb_len)
    string(REGEX MATCHALL "[0-9A-Fa-f][0-9A-Fa-f]" tb_bytes "${tb_uuid}")
    list(LENGTH tb_bytes tb_n)
    if(NOT tb_len EQUAL 32 OR NOT tb_n EQUAL 16)
      message(FATAL_ERROR "CONFIG_TINYBLE_TINYTALK_SCAN_RSP_UUID128 is not 32 hex digits: "
                          "${CONFIG_TINYBLE_TINYTALK_SCAN_RSP_UUID128}")
    endif()
    list(REVERSE tb_bytes)
    list(TRANSFORM tb_bytes PREPEND "0x")
    list(JOIN tb_bytes "," tb_bytes)
    zephyr_library_compile_definitions(TB_SCAN_RSP=1 TB_TINYTALK_SCAN_RSP_UUID128=${tb_bytes})
  endif()
endif()
