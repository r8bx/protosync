/**
 * ads1050.cpp
 *
 * ADS1015 driver using Linux i2c-dev.
 *
 * The ADS1015 is a SIGNED 12-bit ADC. Full-scale range is -2048..+2047.
 * For single-ended inputs the negative half is unused, so usable range
 * is 0..2047 — NOT 0..4095. The raw 16-bit register value must be sign-
 * extended with an arithmetic right-shift before use.
 *
 * Register protocol:
 *   Write config:  [0x01, config_hi, config_lo]
 *   Point to conv: [0x00]
 *   Read result:   2 bytes → sign-extend → arithmetic shift right 4
 */

#include "ads1050.h"
#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

/* ── ADS1015 register pointers ───────────────────────────────────────────── */
static constexpr uint8_t REG_CONVERSION = 0x00;
static constexpr uint8_t REG_CONFIG     = 0x01;

/* ── Config register ─────────────────────────────────────────────────────── */
// Bit 15    : OS  — write 1 to start single-shot conversion
// Bits 14-12: MUX — single-ended: 100=CH0 101=CH1 110=CH2 111=CH3
// Bits 11-9 : PGA — 001 = ±4.096 V
// Bit 8     : MODE — 1 = single-shot
// Bits 7-5  : DR  — 100 = 1600 SPS
// Bits 4-0  : comparator disabled

static constexpr uint8_t CONFIG_LO = 0x83;   /* MODE=1 DR=100 comp=off */

// High byte per channel: OS=1, MUX=1xx, PGA=001
static constexpr uint8_t CONFIG_HI[4] = {
    0xC3,   // CH0: OS=1 MUX=100 PGA=001
    0xD3,   // CH1: OS=1 MUX=101 PGA=001
    0xE3,   // CH2: OS=1 MUX=110 PGA=001
    0xF3,   // CH3: OS=1 MUX=111 PGA=001
};

/* ─────────────────────────────────────────────────────────────────────────
 * init
 * ───────────────────────────────────────────────────────────────────────── */
bool Ads1050::init()
{
    closeIfOpen();

    fd_ = open(bus.c_str(), O_RDWR);
    if (fd_ < 0) {
        fprintf(stderr, "[ADC] open(%s) failed: %s\n", bus.c_str(), strerror(errno));
        return false;
    }

    if (ioctl(fd_, I2C_SLAVE, address) < 0) {
        fprintf(stderr, "[ADC] ioctl I2C_SLAVE 0x%02x failed: %s\n",
                address, strerror(errno));
        closeIfOpen();
        return false;
    }

    return true;
}

/* ─────────────────────────────────────────────────────────────────────────
 * readChannel
 *
 * Returns 0..2047 for single-ended inputs (ADS1015 is a signed ADC,
 * positive half-range only is usable for single-ended measurements).
 * Returns nullopt on I2C error.
 * ───────────────────────────────────────────────────────────────────────── */
std::optional<uint16_t> Ads1050::readChannel(int channel)
{
    if (fd_ < 0 || channel < 0 || channel > 3)
        return std::nullopt;

    /* 1. Write config — select channel, start single-shot conversion */
    uint8_t config_cmd[3] = { REG_CONFIG, CONFIG_HI[channel], CONFIG_LO };
    if (write(fd_, config_cmd, 3) != 3) {
        fprintf(stderr, "[ADC] config write CH%d failed: %s\n",
                channel, strerror(errno));
        return std::nullopt;
    }

    /* 2. Wait for conversion (1600 SPS → 625 µs/sample, 2 ms is safe) */
    usleep(2000);

    /* 3. Point register pointer at conversion register */
    uint8_t ptr = REG_CONVERSION;
    if (write(fd_, &ptr, 1) != 1) {
        fprintf(stderr, "[ADC] pointer write CH%d failed: %s\n",
                channel, strerror(errno));
        return std::nullopt;
    }

    /* 4. Read 2 bytes, MSB first */
    uint8_t buf[2] = { 0, 0 };
    if (read(fd_, buf, 2) != 2) {
        fprintf(stderr, "[ADC] read CH%d failed: %s\n",
                channel, strerror(errno));
        return std::nullopt;
    }

    /* 5. Sign-extend then arithmetic right-shift 4.
     *
     *    The ADS1015 result occupies bits 15-4 of the 16-bit register.
     *    Casting to int16_t before shifting preserves the sign bit —
     *    this is the critical difference from a uint16_t shift which
     *    would give garbage values of 4080+ for anything near zero.
     *
     *    Result range: -2048..+2047
     *    Single-ended inputs stay in 0..+2047, so we clamp negatives
     *    (below-GND noise) to 0.
     */
    int16_t raw = static_cast<int16_t>((buf[0] << 8) | buf[1]);
    raw >>= 4;                             /* arithmetic shift, keeps sign  */
    if (raw < 0) raw = 0;                  /* clamp sub-GND noise to 0      */

    return static_cast<uint16_t>(raw);     /* 0..2047                       */
}

/* ─────────────────────────────────────────────────────────────────────────
 * readAllChannels
 * ───────────────────────────────────────────────────────────────────────── */
std::array<std::optional<uint16_t>, 4> Ads1050::readAllChannels()
{
    std::array<std::optional<uint16_t>, 4> out;
    for (int ch = 0; ch < 4; ++ch)
        out[ch] = readChannel(ch);
    return out;
}

/* ─────────────────────────────────────────────────────────────────────────
 * closeIfOpen
 * ───────────────────────────────────────────────────────────────────────── */
void Ads1050::closeIfOpen()
{
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}
