# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.3.0)
set(toolchainVersion 15_2_Rel1)
set(picotoolVersion 2.3.0)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

# Pull in Raspberry Pi Pico SDK (must be before project)
include(pico_sdk_import.cmake)

project(agent_fingers C CXX ASM)

# Initialise the Raspberry Pi Pico SDK
pico_sdk_init()

# Embed git hash + build timestamp into firmware/generated/build_info.h,
# read by protocol/dispatch.c for GET_INFO.
execute_process(
        COMMAND git describe --always --dirty --abbrev=8
        WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
        OUTPUT_VARIABLE FW_GIT_HASH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
)
if (NOT FW_GIT_HASH)
    set(FW_GIT_HASH "unknown")
endif()
string(TIMESTAMP FW_BUILD_TIMESTAMP_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
configure_file(
        ${CMAKE_CURRENT_LIST_DIR}/firmware/generated/build_info.h.in
        ${CMAKE_BINARY_DIR}/generated/build_info.h
        @ONLY
)

add_executable(agent_fingers
        firmware/app/main.c
        firmware/app/board.c
        firmware/io/pin_claims.c
        firmware/io/safe_state.c
        firmware/io/digital_bank.c
        firmware/peripherals/adc_scan.c
        firmware/peripherals/pwm_out.c
        firmware/peripherals/uart_port.c
        firmware/peripherals/i2c_bus.c
        firmware/peripherals/spi_master.c
        firmware/peripherals/spi_slave.c
        firmware/protocol/crc32c.c
        firmware/protocol/dispatch.c
        firmware/transport/usb_descriptors.c
        firmware/transport/usb_transport.c
        firmware/transport/usb_cdc.c
        firmware/transport/usb_vendor.c
)

pico_set_program_name(agent_fingers "agent_fingers")
pico_set_program_version(agent_fingers "0.1")

# Firmware owns USB via its own TinyUSB composite device (transport/);
# do not let SDK stdio-over-UART/USB contend with it.
pico_enable_stdio_uart(agent_fingers 0)
pico_enable_stdio_usb(agent_fingers 0)

pico_generate_pio_header(agent_fingers ${CMAKE_CURRENT_LIST_DIR}/firmware/pio/digital_io.pio)
pico_generate_pio_header(agent_fingers ${CMAKE_CURRENT_LIST_DIR}/firmware/pio/spi_master.pio)
pico_generate_pio_header(agent_fingers ${CMAKE_CURRENT_LIST_DIR}/firmware/pio/spi_slave.pio)

target_include_directories(agent_fingers PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}/firmware
        ${CMAKE_CURRENT_LIST_DIR}/firmware/transport
        ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(agent_fingers
        pico_stdlib
        pico_unique_id
        pico_rand
        hardware_watchdog
        hardware_pio
        hardware_dma
        hardware_clocks
        hardware_adc
        hardware_pwm
        hardware_uart
        hardware_i2c
        pico_i2c_slave
        tinyusb_device
)

pico_add_extra_outputs(agent_fingers)

