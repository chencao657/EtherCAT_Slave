cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME EtherCAT_Slave_STD)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

target_sources(${CMAKE_PROJECT_NAME} PRIVATE
   
    App/Src/ecat_io_app.c
    BSP/Src/bsp_gpio.c
    BSP/Src/bsp_key.c
    BSP/Src/bsp_lan9253.c
    BSP/Src/bsp_led.c
    BSP/Src/bsp_spi.c
    BSP/Src/bsp_ssc_hw.c
    BSP/Src/bsp_uart.c
    BSP/Src/ring_buffer.c
    Test/Src/test_app.c
    Test/Src/test_lan9253.c
    EtherCAT/Src/coeappl.c
    EtherCAT/Src/ecatappl.c
    EtherCAT/Src/ecatcoe.c
    EtherCAT/Src/ecatslv.c
    EtherCAT/Src/CP-Device.c
    EtherCAT/Src/mailbox.c
    EtherCAT/Src/objdef.c
    EtherCAT/Src/sdoserv.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
  
    BSP/Inc
    App/Inc
    Test/Inc
    EtherCAT/Inc
)

target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-include>
    $<$<COMPILE_LANGUAGE:C>:${CMAKE_CURRENT_SOURCE_DIR}/BSP/Inc/bsp_ssc_hw.h>
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
    ECAT_TIMER_INC_P_MS=1
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
