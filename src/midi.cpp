/**
 * midi.cpp
 * MidiPort implementation.
 *
 * Uses Linux termios2 + BOTHER ioctl to set the non-standard 31250 baud rate
 * required by MIDI without relying on pigpio's serial API.
 *
 * NOTE: do NOT include <termios.h> in this translation unit — it conflicts
 * with <asm/termbits.h> which provides struct termios2 / BOTHER.
 */

#include "midi.hpp"

#include <asm/termbits.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>

#include <spdlog/spdlog.h>

/* ─────────────────────────────────────────────────────────────────────────
 * MidiPort::open
 *
 * Opens the UART, configures 31250 baud 8N1 raw mode via termios2.
 * The port is opened O_RDWR | O_NONBLOCK so that poll() never blocks
 * when the RX FIFO is empty, and TX writes go straight to the kernel
 * buffer without waiting for drain.
 * ───────────────────────────────────────────────────────────────────────── */
bool MidiPort::open(const std::string &device)
{
    close();

    fd_ = ::open(device.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd_ < 0) {
        spdlog::error("MIDI: open({}) failed: {}", device, strerror(errno));
        return false;
    }

    /* Read current settings then overwrite with 8N1 raw at 31250 baud */
    struct termios2 t2 = {};
    if (ioctl(fd_, TCGETS2, &t2) < 0) {
        spdlog::error("MIDI: TCGETS2 failed: {}", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    /* Clear baud, character size, stop bits, parity, flow-control bits */
    t2.c_cflag &= ~(CBAUD | CSIZE | CSTOPB | PARENB | CRTSCTS);

    /* BOTHER  — use c_ispeed/c_ospeed for the actual rate
     * CS8     — 8 data bits
     * CREAD   — enable receiver
     * CLOCAL  — ignore modem control lines                               */
    t2.c_cflag |= BOTHER | CS8 | CREAD | CLOCAL;

    /* Input: ignore break, parity/frame errors; no software flow control */
    t2.c_iflag = IGNBRK | IGNPAR;

    /* Output and local flags: raw (no processing) */
    t2.c_oflag = 0;
    t2.c_lflag = 0;

    /* Non-blocking reads: return immediately if no data */
    t2.c_cc[VMIN]  = 0;
    t2.c_cc[VTIME] = 0;

    t2.c_ispeed = 31250;
    t2.c_ospeed = 31250;

    if (ioctl(fd_, TCSETS2, &t2) < 0) {
        spdlog::error("MIDI: TCSETS2 (31250 baud) failed: {}", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    /* Flush any stale bytes in the kernel buffer before we start */
    ioctl(fd_, TCFLSH, TCIOFLUSH);

    device_ = device;
    spdlog::info("MIDI: {} opened — 31250 baud 8N1 (TX=GPIO14 OUT, RX=GPIO15 IN)",
                 device);
    return true;
}

/* ─────────────────────────────────────────────────────────────────────────
 * MidiPort::close
 * ───────────────────────────────────────────────────────────────────────── */
void MidiPort::close()
{
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        spdlog::debug("MIDI: {} closed", device_);
        device_.clear();
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * MidiPort::sendByte  (private)
 * ───────────────────────────────────────────────────────────────────────── */
void MidiPort::sendByte(uint8_t b)
{
    if (fd_ < 0) return;
    /* write() on O_NONBLOCK serial is effectively non-blocking for tiny
     * payloads — the kernel TX buffer is many kilobytes, and MIDI clock
     * sends at most ~48 bytes/second.                                    */
    char c = static_cast<char>(b);
    if (::write(fd_, &c, 1) != 1)
        spdlog::warn("MIDI: TX write failed: {}", strerror(errno));
}

/* ── TX helpers ──────────────────────────────────────────────────────────── */
void MidiPort::sendClock()    { sendByte(MIDI_CLOCK);    }
void MidiPort::sendStart()    { sendByte(MIDI_START);    }
void MidiPort::sendStop()     { sendByte(MIDI_STOP);     }
void MidiPort::sendContinue() { sendByte(MIDI_CONTINUE); }

/* ─────────────────────────────────────────────────────────────────────────
 * MidiPort::dataBytes  (static private)
 *
 * Returns the number of data bytes expected after the status byte:
 *   0   system real-time or Tune Request (single/zero data byte message)
 *  -1   SysEx start/end or undefined — discard and reset parser
 *  1/2  normal channel or system-common message
 * ───────────────────────────────────────────────────────────────────────── */
int MidiPort::dataBytes(uint8_t s)
{
    if (s >= 0xF8) return  0;   /* System Real-Time: 0xF8-0xFF */
    if (s == 0xF7) return -1;   /* SysEx End                   */
    if (s == 0xF0) return -1;   /* SysEx Start                 */
    if (s == 0xF4 ||
        s == 0xF5 ||
        s == 0xFD) return -1;   /* Undefined                   */
    if (s == 0xF1) return  1;   /* MTC Quarter Frame           */
    if (s == 0xF2) return  2;   /* Song Position Pointer       */
    if (s == 0xF3) return  1;   /* Song Select                 */
    if (s == 0xF6) return  0;   /* Tune Request                */

    /* Channel messages */
    switch (s & 0xF0) {
        case MIDI_NOTE_OFF:   return 2;
        case MIDI_NOTE_ON:    return 2;
        case MIDI_POLY_AT:    return 2;
        case MIDI_CC:         return 2;
        case MIDI_PROG:       return 1;
        case MIDI_CHAN_AT:    return 1;
        case MIDI_PITCHBEND:  return 2;
        default:              return -1;
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * MidiPort::poll
 *
 * Drains all bytes currently in the OS RX FIFO through the running-status
 * MIDI parser.  Returns true if at least one complete message (channel or
 * transport/RT) was decoded during this call.
 *
 * System Real-Time bytes (0xF8–0xFF) are handled inline per MIDI 1.0
 * spec §2.2 — they never interrupt or reset the running-status parser.
 * Transport events (Start, Stop, Continue, Clock) are signalled via
 * MidiState flags; channel messages populate the status/data fields.
 *
 * System Common messages (0xF0–0xF6) cancel running status but are NOT
 * forwarded as channel messages.
 * ───────────────────────────────────────────────────────────────────────── */
bool MidiPort::poll(MidiState &out)
{
    if (fd_ < 0) return false;

    /* Clear per-call transport flags — they are edge signals, not levels */
    out.clock_tick         = false;
    out.transport_start    = false;
    out.transport_stop     = false;
    out.transport_continue = false;

    bool got_message = false;

    for (;;) {
        uint8_t byte;
        ssize_t n = ::read(fd_, &byte, 1);
        if (n <= 0) break;   /* EAGAIN / no data — done for this call */

        /* ── System Real-Time (0xF8–0xFF) ───────────────────────────────
         * Single-byte, no data bytes, never affects running status.
         * May arrive in the middle of any other message — handle and
         * continue assembling whatever was in progress.                  */
        if (byte >= 0xF8) {
            switch (byte) {
                case MIDI_CLOCK:
                    out.clock_tick = true;
                    spdlog::debug("MIDI RX: F8 (Clock)");
                    break;
                case MIDI_START:
                    out.transport_start = true;
                    spdlog::info("MIDI RX: FA — Start");
                    break;
                case MIDI_STOP:
                    out.transport_stop = true;
                    spdlog::info("MIDI RX: FC — Stop");
                    break;
                case MIDI_CONTINUE:
                    out.transport_continue = true;
                    spdlog::info("MIDI RX: FB — Continue");
                    break;
                default:
                    /* 0xF9, 0xFD, 0xFE (Active Sensing), 0xFF (Reset) */
                    spdlog::debug("MIDI RX: {:02X} (undefined RT)", byte);
                    break;
            }
            got_message = true;
            continue;   /* do NOT touch parser state */
        }

        /* ── New status byte ────────────────────────────────────────────── */
        if (byte & 0x80) {
            int nb = dataBytes(byte);

            if (nb < 0) {
                /* SysEx or undefined system common — reset parser */
                spdlog::debug("MIDI RX: {:02X} (SysEx/undefined, parser reset)", byte);
                ps_status_      = 0;
                ps_count_       = 0;
                ps_expected_    = 0;
                ps_is_syscommon_ = false;
                continue;
            }

            if (nb == 0) {
                /* Tune Request (0xF6) — no data bytes, no running status */
                spdlog::debug("MIDI RX: {:02X} (Tune Request)", byte);
                ps_status_      = 0;
                ps_count_       = 0;
                ps_expected_    = 0;
                ps_is_syscommon_ = false;
                continue;
            }

            /* System Common with data bytes (F1, F2, F3):
             * These cancel running status and are parsed but NOT
             * forwarded as channel messages.                            */
            ps_is_syscommon_ = (byte >= 0xF0 && byte <= 0xF6);

            ps_status_   = byte;
            ps_expected_ = nb;
            ps_count_    = 0;
            continue;
        }

        /* ── Data byte ──────────────────────────────────────────────────── */
        if (ps_status_ == 0 || ps_expected_ == 0)
            continue;   /* no running status established yet */

        ps_buf_[ps_count_++] = byte;
        if (ps_count_ < ps_expected_)
            continue;   /* wait for remaining data bytes */

        /* ── Complete message ───────────────────────────────────────────── */
        uint8_t status = ps_status_;
        uint8_t d1     = ps_buf_[0];
        uint8_t d2     = (ps_expected_ == 2) ? ps_buf_[1] : 0u;

        /* Reset data counter, keep running status for channel messages.
         * System Common messages cancel running status after completion. */
        ps_count_ = 0;
        if (ps_is_syscommon_) {
            spdlog::debug("MIDI RX: {:02X} {:02X} {:02X}  (system common, discarded)",
                          status, d1, d2);
            ps_status_       = 0;
            ps_expected_     = 0;
            ps_is_syscommon_ = false;
            continue;   /* do not forward as a channel message */
        }

        /* Channel message — populate MidiState */
        uint8_t ch   = static_cast<uint8_t>((status & 0x0Fu) + 1u);  /* 1..16 */
        uint8_t type = status & 0xF0u;

        out.status  = status;
        out.data1   = d1;
        out.data2   = d2;
        out.channel = ch;

        if (type == MIDI_CC) {
            out.cc     = d1;
            out.cc_val = d2;
        }

        out.rx_count++;
        got_message = true;

        spdlog::debug("MIDI RX: {:02X} {:02X} {:02X}  ch{}",
                      status, d1, d2, ch);
    }

    return got_message;
}

/* ─────────────────────────────────────────────────────────────────────────
 * MidiPort::txTest
 *
 * Transmit test loop — sends Start, waits @p interval_ms, sends Stop,
 * waits @p interval_ms, then repeats.  Blocks until @p running is set
 * to false from another thread (e.g. signal handler).
 *
 * Example usage in main():
 *
 *   std::atomic<bool> running{true};
 *   signal(SIGINT, [](int){ running = false; });
 *   midi.txTest(running, 2000);   // toggle every 2 seconds
 * ───────────────────────────────────────────────────────────────────────── */
void MidiPort::txTest(std::atomic<bool> &running, int interval_ms)
{
    /* C3 = note 60 (Ableton convention), channel 1 */
    static constexpr uint8_t NOTE     = 60;
    static constexpr uint8_t VELOCITY = 100;
    static constexpr uint8_t NOTE_ON  = 0x90;   /* ch1 Note On  */
    static constexpr uint8_t NOTE_OFF = 0x80;   /* ch1 Note Off */

    spdlog::info("MIDI TX test: Start → C3 NoteOn → {}ms → NoteOff → Stop → repeat",
                 interval_ms);

    while (running) {
        /* Start */
        sendStart();
        spdlog::info("MIDI TX: FA (Start)");

        /* Note On — C3 ch1 */
        sendByte(NOTE_ON);
        sendByte(NOTE);
        sendByte(VELOCITY);
        spdlog::info("MIDI TX: 90 3C 64 (Note On C3 vel=100 ch1)");

        /* Hold note */
        for (int i = 0; i < interval_ms && running; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));

        if (!running) break;

        /* Note Off — C3 ch1 */
        sendByte(NOTE_OFF);
        sendByte(NOTE);
        sendByte(0x00);
        spdlog::info("MIDI TX: 80 3C 00 (Note Off C3 ch1)");

        /* Stop */
        sendStop();
        spdlog::info("MIDI TX: FC (Stop)");

        /* Gap before next cycle */
        for (int i = 0; i < interval_ms && running; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    spdlog::info("MIDI TX test: stopped");
}