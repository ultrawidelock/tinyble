# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# tinytalk's door-lock on tinyble. Set TINYTALK=<a tinytalk tree> in the
# environment (sysbuild does not pass -DTINYTALK on to this image).
if(NOT TINYTALK)
  set(TINYTALK $ENV{TINYTALK})
endif()
if(NOT TINYTALK)
  message(FATAL_ERROR "set TINYTALK=<a tinytalk tree>")
endif()
list(APPEND ZEPHYR_EXTRA_MODULES ${CMAKE_CURRENT_SOURCE_DIR}/../.. ${TINYTALK})
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(tinyble_door_lock)

target_sources(app PRIVATE src/main.c)

# The manual pairing code, for the boot log. tinytalk's script also stops the
# build when the verifier in .config does not match the passcode.
execute_process(
  COMMAND ${PYTHON_EXECUTABLE} ${TINYTALK}/scripts/spake2p_verifier.py --from-config ${DOTCONFIG}
  OUTPUT_VARIABLE TB_PAIRING_OUT
  ERROR_VARIABLE TB_PAIRING_ERR
  RESULT_VARIABLE TB_PAIRING_RC)
string(REGEX MATCH "setup code  ([0-9]+-[0-9]+-[0-9]+)" TB_PAIRING_MATCH "${TB_PAIRING_OUT}")
if(NOT TB_PAIRING_RC EQUAL 0 OR NOT CMAKE_MATCH_1)
  message(FATAL_ERROR "${TB_PAIRING_OUT}${TB_PAIRING_ERR}")
endif()
message(STATUS "manual pairing code: ${CMAKE_MATCH_1}")
target_compile_definitions(app PRIVATE MANUAL_CODE="${CMAKE_MATCH_1}")
