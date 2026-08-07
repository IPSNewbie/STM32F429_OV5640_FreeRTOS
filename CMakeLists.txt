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
set(CMAKE_PROJECT_NAME ISP_OV5640)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME}
        BSPDrivers/Inc/bsp_log.h
        BSPDrivers/Src/bsp_log.c
        BSPDrivers/Inc/bsp_PCF8574.h
        BSPDrivers/Src/bsp_PCF8574.c
        BSPDrivers/Inc/bsp_sccb.h
        BSPDrivers/Src/bsp_sccb.c
        BSPDrivers/Inc/bsp_softiic.h
        BSPDrivers/Src/bsp_softiic.c
        BSPDrivers/Inc/delay_tim.h
        BSPDrivers/Src/delay_tim.c
        BSPDrivers/Inc/camera_dcmi_dma.h
        BSPDrivers/Src/camera_dcmi_dma.c
        BSPDrivers/Inc/camera_cli.h
        BSPDrivers/Src/camera_cli.c
        BSPDrivers/Inc/camera_sd_storage.h
        BSPDrivers/Src/camera_sd_storage.c
        BSPDrivers/Inc/camera_fatfs_diskio.h
        BSPDrivers/Src/camera_fatfs_diskio.c
        BSPDrivers/Inc/camera_snapshot_control.h
        BSPDrivers/Src/camera_snapshot_control.c
        BSPDrivers/Inc/camera_frame_buffer.h
        BSPDrivers/Src/camera_frame_buffer.c
        BSPDrivers/Inc/camera_image_process.h
        BSPDrivers/Src/camera_image_process.c
        BSPDrivers/Inc/lcd_mcu.h
        BSPDrivers/Src/lcd_mcu.c
        BSPDrivers/Inc/OV5640.h
        BSPDrivers/Src/OV5640.c
        BSPDrivers/Inc/OV5640cfg.h
        BSPDrivers/Src/lcd_mcu_init.c
        BSPDrivers/Inc/lcd_mcu_init.h
)

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

file(GLOB_RECURSE BSPDRIVERS CONFIGURE_DEPENDS "BSPDrivers/Src/*.c")
# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
        # Add user sources here
        ${BSPDRIVERS}
        Middlewares/Third_Party/FatFs/src/ff.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dcmi.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_sd.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_sdmmc.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_sram.c
        Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_ll_fmc.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined include paths
        BSPDrivers/Inc
        Middlewares/Third_Party/FatFs/src
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
        HAL_DCMI_MODULE_ENABLED
        HAL_SRAM_MODULE_ENABLED
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
