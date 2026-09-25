#
# SPDX-License-Identifier: Apache-2.0
#
# Builds build/battery_switch.uf2 for the XIAO's Adafruit UF2 bootloader. It
# contains the application and the Matter factory data, so a single file
# copied to the bootloader drive (e.g. XIAO-BOOT) installs everything.
#

set(battery_switch_app_hex ${CMAKE_BINARY_DIR}/${DEFAULT_IMAGE}/zephyr/zephyr.hex)
set(battery_switch_fd_hex ${CMAKE_BINARY_DIR}/matter_factory_data/zephyr/factory_data.hex)
set(battery_switch_merged_hex ${CMAKE_BINARY_DIR}/battery_switch.hex)
set(battery_switch_uf2 ${CMAKE_BINARY_DIR}/battery_switch.uf2)

# nRF52840 UF2 family ID, as used by the Adafruit nRF52 bootloader.
set(battery_switch_uf2_family 0xADA52840)

if(SB_CONFIG_MATTER_FACTORY_DATA_GENERATE)
  set(battery_switch_hex_inputs ${battery_switch_app_hex} ${battery_switch_fd_hex})
else()
  set(battery_switch_hex_inputs ${battery_switch_app_hex})
endif()

# Always regenerated (it only takes a moment), so it follows image rebuilds.
add_custom_target(battery_switch_uf2 ALL
  COMMAND ${PYTHON_EXECUTABLE} ${ZEPHYR_BASE}/scripts/build/mergehex.py
          -o ${battery_switch_merged_hex} ${battery_switch_hex_inputs}
  COMMAND ${PYTHON_EXECUTABLE} ${ZEPHYR_BASE}/scripts/build/uf2conv.py
          -c -f ${battery_switch_uf2_family}
          -o ${battery_switch_uf2} ${battery_switch_merged_hex}
  BYPRODUCTS ${battery_switch_merged_hex} ${battery_switch_uf2}
  COMMENT "Generating battery_switch.uf2 (application + Matter factory data)"
)

add_dependencies(battery_switch_uf2 ${DEFAULT_IMAGE})
if(SB_CONFIG_MATTER_FACTORY_DATA_GENERATE)
  add_dependencies(battery_switch_uf2 matter_factory_data)
endif()
