#
# SPDX-License-Identifier: Apache-2.0
#
# Builds build/battery_switch_app.uf2 for the Adafruit UF2 bootloader. It
# contains the application only, never Matter factory data, so the same file
# works for every device: a device creates its own onboarding credentials on
# first boot (src/transport/matter/self_provision.cpp), or gets them from a
# per-device image made by tools/provision_device.py. Firmware updates with
# this file leave the factory data page, and so the pairing code, untouched.
#

# Software version shown by Matter controllers: the tag if HEAD is tagged,
# otherwise the short commit hash.
find_package(Git QUIET)
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --exact-match HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE fw_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(NOT fw_version)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
      WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
      OUTPUT_VARIABLE fw_version OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  endif()
  if(fw_version)
    set_config_string(${DEFAULT_IMAGE} CONFIG_CHIP_DEVICE_SOFTWARE_VERSION_STRING "${fw_version}")
  endif()

  # Configure again when HEAD moves or tags change.
  execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --absolute-git-dir
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE git_dir OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  execute_process(COMMAND ${GIT_EXECUTABLE} symbolic-ref -q HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
    OUTPUT_VARIABLE git_branch_ref OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  foreach(f HEAD ${git_branch_ref} packed-refs refs/tags)
    if(git_dir AND EXISTS ${git_dir}/${f})
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${git_dir}/${f})
    endif()
  endforeach()
endif()

set(battery_switch_app_hex ${CMAKE_BINARY_DIR}/${DEFAULT_IMAGE}/zephyr/zephyr.hex)
set(battery_switch_app_uf2 ${CMAKE_BINARY_DIR}/battery_switch_app.uf2)

# nRF52840 UF2 family ID, as used by the Adafruit nRF52 bootloader.
set(battery_switch_uf2_family 0xADA52840)

# Always regenerated (it only takes a moment), so it follows image rebuilds.
add_custom_target(battery_switch_uf2 ALL
  COMMAND ${PYTHON_EXECUTABLE} ${ZEPHYR_BASE}/scripts/build/uf2conv.py
          -c -f ${battery_switch_uf2_family}
          -o ${battery_switch_app_uf2} ${battery_switch_app_hex}
  BYPRODUCTS ${battery_switch_app_uf2}
  COMMENT "Generating battery_switch_app.uf2 (application only)"
)

add_dependencies(battery_switch_uf2 ${DEFAULT_IMAGE})
