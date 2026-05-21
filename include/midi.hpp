#pragma once
/**
 * midi.hpp
 * MidiPort — UART-based MIDI I/O at 31250 baud (8N1).
 *
 * Hardware wiring (Raspberry Pi):
 *   MIDI OUT  TX → GPIO14  (/dev/ttyAMA0)  — clock, start, stop
 *   MIDI IN   RX ← GPIO15  (/dev/ttyAMA0)  — all channel messages
 *
 * For a second physical MIDI IN on separate pins, open a second MidiPort
 * on /dev/ttyAMA1 (GPIO0/GPIO1, requires enable_uart=1 in config.txt).
 *
 * Prerequisites (add to /boot/firmware/config.txt if not already present):
 *   enable_uart=1
 *   dtoverlay=disable-bt          # frees ttyAMA0 from Bluetooth on Pi 3/4/5
 *
 * Uses termios2 / BOTHER for non-standard 31250 baud — no pigpio serial
 * functions needed, only the GPIO subsystem already initialised by main().
 */

#include <cstdint>
#include <string>
#include <atomic>
#include <thread>
#include <chrono>

/* ── System Real-Time byte constants ─────────────────────────────────────── */
inline constexpr uint8_t MIDI_CLOCK    = 0xF8;
inline constexpr uint8_t MIDI_START    = 0xFA;
inline constexpr uint8_t MIDI_CONTINUE = 0xFB;
inline constexpr uint8_t MIDI_STOP     = 0xFC;

/* ── Channel message type nibbles ────────────────────────────────────────── */
inline constexpr uint8_t MIDI_NOTE_OFF   = 0x80;
inline constexpr uint8_t MIDI_NOTE_ON    = 0x90;
inline constexpr uint8_t MIDI_POLY_AT    = 0xA0;
inline constexpr uint8_t MIDI_CC         = 0xB0;
inline constexpr uint8_t MIDI_PROG       = 0xC0;
inline constexpr uint8_t MIDI_CHAN_AT    = 0xD0;
inline constexpr uint8_t MIDI_PITCHBEND  = 0xE0;

/* ─────────────────────────────────────────────────────────────────────────
 * MidiState — shared snapshot between the MIDI threads and the render loop.
 * ───────────────────────────────────────────────────────────────────────── */
struct MidiState {
    /* RX: last decoded channel message */
    uint8_t  status      = 0x00;
    uint8_t  data1       = 0x00;
    uint8_t  data2       = 0x00;
    uint8_t  channel     = 0x00;   ///< 1..16
    uint8_t  cc          = 0x00;   ///< last CC number (type 0xB0 only)
    uint8_t  cc_val      = 0x00;   ///< last CC value
    uint32_t rx_count    = 0;      ///< total channel messages received

    /* RX: transport / real-time flags.
     * Set by poll() when the corresponding RT byte is received.
     * Cleared at the TOP of each poll() call — treat as edge signals,
     * not levels.  Check them immediately after poll() returns true.  */
    bool clock_tick         = false;  ///< 0xF8 received this poll cycle
    bool transport_start    = false;  ///< 0xFA received this poll cycle
    bool transport_stop     = false;  ///< 0xFC received this poll cycle
    bool transport_continue = false;  ///< 0xFB received this poll cycle

    /* TX: written by the MIDI TX thread */
    bool     clk_running = false;
    uint32_t clk_count   = 0;      ///< 0xF8 bytes sent since start
};

/* ─────────────────────────────────────────────────────────────────────────
 * MidiPort
 *
 * Thread-safety: open/close must happen from a single owner thread.
 * sendByte/sendClock/sendStart/sendStop: safe to call from one TX thread.
 * poll(): safe to call from one RX thread.
 * Do NOT call TX and RX concurrently without external locking (full-duplex
 * on the same fd is fine at the OS level but the parser state is not
 * thread-safe).  The design in main.cpp uses separate TX and RX threads
 * that each only call their respective half.
 * ───────────────────────────────────────────────────────────────────────── */
class MidiPort {
public:
    MidiPort()  = default;
    ~MidiPort() { close(); }

    MidiPort(const MidiPort &)            = delete;
    MidiPort &operator=(const MidiPort &) = delete;

    /**
     * Open the UART at 31250 baud, 8N1, raw mode (no flow control).
     * Uses termios2 / BOTHER so the non-standard rate is set precisely.
     * Returns true on success.  Logs the failure reason on error.
     */
    bool open(const std::string &device = "/dev/ttyAMA0");

    /** Close the file descriptor.  Safe to call when already closed. */
    void close();

    bool isOpen() const { return fd_ >= 0; }
    const std::string &device() const { return device_; }

    /* ── TX ─────────────────────────────────────────────────────────────── */

    /** Send a single System Real-Time clock tick (0xF8). */
    void sendClock();

    /** Send MIDI Start (0xFA). */
    void sendStart();

    /** Send MIDI Stop (0xFC). */
    void sendStop();

    /** Send MIDI Continue (0xFB). */
    void sendContinue();

    /**
     * Blocking TX test loop — alternates Start/Stop every @p interval_ms.
     * Set @p running to false from a signal handler to exit cleanly.
     */
    void txTest(std::atomic<bool> &running, int interval_ms = 2000);

    /* ── RX ─────────────────────────────────────────────────────────────── */

    /**
     * Drain all bytes currently in the RX FIFO through the running-status
     * MIDI parser.  Returns true if at least one complete message was decoded
     * (channel message or RT transport event); @p out receives the last
     * decoded channel message plus any transport flags set this cycle.
     *
     * System Real-Time bytes (0xF8–0xFF) do not affect the running-status
     * parser (per MIDI 1.0 spec §2.2) and are reported via MidiState flags.
     * System Common messages (0xF0–0xF6) cancel running status but are not
     * forwarded as channel messages.
     *
     * Call from a dedicated RX thread (e.g., with usleep(1000) between calls).
     */
    bool poll(MidiState &out);

private:
    int         fd_     = -1;
    std::string device_;

    /** Write one byte to the TX FIFO.  No-op if not open. */
    void sendByte(uint8_t b);

    /* Running-status parser ─────────────────────────────────────────────── */
    uint8_t ps_status_        = 0;     ///< current status byte (running status)
    uint8_t ps_buf_[2]        = {};    ///< accumulated data bytes
    int     ps_count_         = 0;     ///< data bytes received so far
    int     ps_expected_      = 0;     ///< data bytes expected for ps_status_
    bool    ps_is_syscommon_  = false; ///< true when parsing F1/F2/F3 (not a channel msg)

    /**
     * Returns the number of data bytes that follow a given status byte:
     *   ≥1  normal channel / system-common message
     *    0  system real-time (single byte, no data)
     *   -1  SysEx / undefined (skip)
     */
    static int dataBytes(uint8_t status);
};