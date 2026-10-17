# SPDX-License-Identifier: Apache-2.0

if(CONFIG_TINYBLE)
  zephyr_include_directories(../include)
  zephyr_library()
  zephyr_library_sources(../src/tinyble.c ../port/zephyr/tb_port_zephyr.c)
  zephyr_library_sources_ifdef(CONFIG_TINYBLE_MATTER_GATT ../src/tb_matter_gatt.c)
  zephyr_library_sources_ifdef(CONFIG_TINYBLE_TINYTALK ../port/tinytalk/matter_ble_tinyble.c)
  # tinyfota's services in the table, and Find By Type Value to find them.
  zephyr_library_compile_definitions_ifdef(CONFIG_TINYBLE_TINYFOTA TB_MATTER_DFU=1 TB_FIND_BY_TYPE=1)
endif()
