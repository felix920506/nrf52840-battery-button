#
# SPDX-License-Identifier: Apache-2.0
#

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr HINTS $ENV{ZEPHYR_BASE})

project(battery-switch)

target_include_directories(app PRIVATE src)

# Transport-independent core: switch inputs, gesture detection, battery, LED.
target_sources(app PRIVATE
  src/main.c
  src/core/app_loop.c
  src/core/battery.c
  src/core/status_led.c
  src/core/switch_gesture.c
  src/core/switch_input.c
)

target_sources_ifdef(CONFIG_APP_USB_BOOTLOADER_RESET app PRIVATE src/core/usb_bootloader.c)
target_sources_ifdef(CONFIG_APP_TRANSPORT_BLE app PRIVATE src/transport/ble/ble_transport.c)

if(CONFIG_APP_TRANSPORT_MATTER)
  include(${ZEPHYR_CONNECTEDHOMEIP_MODULE_DIR}/config/nrfconnect/app/enable-gnu-std.cmake)
  include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/data_model.cmake)
  include(${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/cmake/zap_helpers.cmake)
  ncs_get_zap_parent_dir(ZAP_PARENT_DIR)

  # Only the parts of the nRF Matter sample commons that do not depend on the
  # DK buttons/LEDs library.
  set(MATTER_COMMONS_SRC_DIR ${ZEPHYR_NRF_MODULE_DIR}/samples/matter/common/src)

  target_include_directories(app PRIVATE
    ${MATTER_COMMONS_SRC_DIR}
    ${ZAP_PARENT_DIR}
  )

  target_sources(app PRIVATE
    ${MATTER_COMMONS_SRC_DIR}/app/matter_init.cpp
    ${MATTER_COMMONS_SRC_DIR}/app/matter_event_handler.cpp
    ${MATTER_COMMONS_SRC_DIR}/clusters/cluster_init.cpp
    src/transport/matter/matter_transport.cpp
  )

  zephyr_linker_sources(SECTIONS ${MATTER_COMMONS_SRC_DIR}/clusters/cluster_init.ld)

  ncs_configure_data_model()
endif()
