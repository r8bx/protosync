/**
 * ads1050.h
 *
 * ADS1015 driver (4-channel, 12-bit ADC) over Linux i2c-dev.
 *
 * Wiring (default):
 *   ADDR pin → GND  →  I2C address 0x48
 *   ADDR pin → VDD  →  I2C address 0x49
 *   ADDR pin → SDA  →  I2C address 0x4A
 *   ADDR pin → SCL  →  I2C address 0x4B
 *
 * All channels are single-ended, ±4.096 V range, 1600 SPS, single-shot.
 * To use ±2.048 V (better for 3.3 V systems) change PGA bits in ads1050.cpp.
 */

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

struct Ads1050 {
    std::string bus     = "/dev/i2c-1";
    uint8_t     address = 0x48;

    Ads1050() = default;
    Ads1050(const std::string &bus_, uint8_t addr_) : bus(bus_), address(addr_) {}

    ~Ads1050() { closeIfOpen(); }

    // Opens the I2C device and sets the slave address. Returns true on success.
    bool init();

    // Read a single channel [0..3]. Returns raw 12-bit value (0–4095) or nullopt on error.
    std::optional<uint16_t> readChannel(int channel);

    // Read all 4 channels. Failed channels return nullopt.
    std::array<std::optional<uint16_t>, 4> readAllChannels();

private:
    int  fd_ = -1;
    void closeIfOpen();
};
