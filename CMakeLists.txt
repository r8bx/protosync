cmake_minimum_required(VERSION 3.16)
project(Protosync CXX C)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS}   -g")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Ścieżka bazowa bibliotek ──────────────────────────────────────────────────
set(LIBS_DIR ${CMAKE_SOURCE_DIR}/libraries)

# ── Ableton Link ──────────────────────────────────────────────────────────────
set(ABLETON_LINK_DIR ${LIBS_DIR}/link)
include(${ABLETON_LINK_DIR}/AbletonLinkConfig.cmake)

# ── Waveshare LCD C sources ───────────────────────────────────────────────────
set(LCD_ROOT ${LIBS_DIR}/waveshare/RaspberryPi/c)
add_library(waveshare_lcd STATIC
    ${LCD_ROOT}/lib/Config/DEV_Config.c
    ${LCD_ROOT}/lib/LCD/LCD_2inch.c
    ${LCD_ROOT}/lib/GUI/GUI_Paint.c
    ${LCD_ROOT}/lib/Fonts/font8.c
    ${LCD_ROOT}/lib/Fonts/font12.c
    ${LCD_ROOT}/lib/Fonts/font16.c
    ${LCD_ROOT}/lib/Fonts/font20.c
    ${LCD_ROOT}/lib/Fonts/font24.c
)
target_include_directories(waveshare_lcd PUBLIC
    ${LCD_ROOT}/lib/Config
    ${LCD_ROOT}/lib/LCD
    ${LCD_ROOT}/lib/GUI
    ${LCD_ROOT}/lib/Fonts
)
find_library(BCM2835_LIB bcm2835)
find_library(WIRINGPI_LIB wiringPi)
if(BCM2835_LIB)
    target_compile_definitions(waveshare_lcd PUBLIC USE_BCM2835_LIB)
    target_link_libraries(waveshare_lcd PUBLIC ${BCM2835_LIB})
elseif(WIRINGPI_LIB)
    target_compile_definitions(waveshare_lcd PUBLIC USE_WIRINGPI_LIB)
    target_link_libraries(waveshare_lcd PUBLIC ${WIRINGPI_LIB})
else()
    message(WARNING "Neither bcm2835 nor wiringPi found")
endif()
target_link_libraries(waveshare_lcd PUBLIC m)

# ── spdlog ────────────────────────────────────────────────────────────────────
set(SPDLOG_DIR ${LIBS_DIR}/spdlog)
if(EXISTS ${SPDLOG_DIR}/CMakeLists.txt)
    add_subdirectory(${SPDLOG_DIR} ${CMAKE_BINARY_DIR}/spdlog)
    set(SPDLOG_TARGET spdlog::spdlog)
else()
    find_package(spdlog REQUIRED)
    set(SPDLOG_TARGET spdlog::spdlog)
endif()

# ── pigpio ────────────────────────────────────────────────────────────────────
set(PIGPIO_DIR ${LIBS_DIR}/pigpio)
if(EXISTS ${PIGPIO_DIR}/CMakeLists.txt)
    add_subdirectory(${PIGPIO_DIR} ${CMAKE_BINARY_DIR}/pigpio)
    set(PIGPIO_LIB pigpio)
else()
    find_library(PIGPIO_LIB pigpio REQUIRED)
endif()

# ── Dependencies ──────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)

# ── protosync ─────────────────────────────────────────────────────────────────
add_executable(protosync
    src/main.cpp
    src/ui.cpp
    src/midi.cpp
    src/ads1050.cpp
    src/logger.cpp
)
target_include_directories(protosync PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${LCD_ROOT}/lib/Config
    ${LCD_ROOT}/lib/LCD
    ${LCD_ROOT}/lib/GUI
    ${LCD_ROOT}/lib/Fonts
    ${ABLETON_LINK_DIR}/include
)
target_link_libraries(protosync PRIVATE
    waveshare_lcd
    Ableton::Link
    Threads::Threads
    ${SPDLOG_TARGET}
    ${PIGPIO_LIB}
    pthread
)