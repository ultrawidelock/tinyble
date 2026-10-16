# SPDX-License-Identifier: Apache-2.0

if(CONFIG_TINYBLE)
  zephyr_include_directories(../include)
  zephyr_library()
  zephyr_library_sources(../src/tinyble.c ../port/zephyr/tb_port_zephyr.c)
  zephyr_library_sources_ifdef(CONFIG_TINYBLE_MATTER_GATT ../src/tb_matter_gatt.c)
endif()
