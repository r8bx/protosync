/**
 * main.cpp
 * Waveshare 2-inch LCD + Ableton Link + ADS1015 ADC + GPIO clock + MIDI I/O
 *
 * Clock / sync outputs (gated by Link transport):
 *   GPIO12 — beat clock (PPQN pulses per quarter note)
 *   GPIO13 — bar pulse  (every PPQN × 4 beats)
 *   GPIO19 — run/stop   (HIGH = playing)
 *
 * MIDI (31250 baud 8N1, /dev/ttyAMA0), gated by Link transport:
 *   GPIO14 TX — OUT: FA on transport-on, FC on transport-off, F8 @ 24 PPQN
 *   GPIO15 RX — IN:  all channel messages (Note, CC, Prog, Bend ...)
 *
 * Buttons (active low, internal pull-up, 10 ms glitch filter):
 *   GPIO5  — ENTER:    open selected page / return to MENU
 *   GPIO23 — RUN:      toggle Link transport on/off (Start/Stop framework-wide)
 *
 * Changes vs original:
 *   [1] clock_thread_fn        — Link-anchored via timeAtBeat(), adaptive pulse width
 *   [2] gpio_measure_thread_fn — sync_error_us computed (was hardcoded 0)
 *   [3] main render loop       — auto-shutdown after 10 000 frames
 *
 * Build:  mkdir -p build && cd build && cmake .. && make -j4
 * Run:    sudo ./build/bin/test_app
 */

#include "ui.hpp"
#include "logger.hpp"
#include "ads1050.h"
#include "midi.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <mutex>
#include <numeric>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <termios.h>
#include <pthread.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

#include <ableton/Link.hpp>
#include <pigpio.h>

/* ── GPIO / clock ────────────────────────────────────────────────────────── */
#define GPIO_BEAT_CLK   12
#define GPIO_BAR_CLK    19
#define GPIO_RUN        13
#define GPIO_BTN_ENTER  5
#define GPIO_BTN_RUN    23
#define CLOCK_PULSE_US  2000u
#define PPQN            4

static constexpr uint32_t MIDI_PPQN   = 24;
static constexpr double   LINK_QUANTUM = 4.0;

/* ── App state ───────────────────────────────────────────────────────────── */
static volatile sig_atomic_t g_running  = 1;
static std::atomic<Page>     g_page     { Page::MENU };
static std::atomic<int>      g_menu_sel { 0 };
static std::atomic<bool>     g_redraw   { true };
static std::atomic<bool>     g_gpio_ok  { false };
static std::atomic<ableton::Link *> g_link_ptr { nullptr };

/* ── Shared Link state ───────────────────────────────────────────────────── */
struct LinkState {
    std::mutex mtx;
    double   bpm        = 120.0;
    double   beat       = 0.0;
    double   phase      = 0.0;
    uint32_t peers      = 0;
    bool     is_playing = false;
    int      bar        = 0;
    double   ms_to_bar  = 0.0;
};
static LinkState g_link_state;

static void on_signal(int) { g_running = 0; }

/* ─────────────────────────────────────────────────────────────────────────
 * btn_enter_cb — GPIO5 ENTER button
 * ───────────────────────────────────────────────────────────────────────── */
static void btn_enter_cb(int /*gpio*/, int level, uint32_t /*tick*/)
{
    if (level != 0) return;
    if (g_page == Page::MENU) {
        g_page.store(static_cast<Page>(g_menu_sel.load() + 1));
        g_redraw.store(true);
        spdlog::info("BTN5 -> open {}", PAGE_NAMES[g_menu_sel.load()]);
    } else {
        g_page.store(Page::MENU);
        g_redraw.store(true);
        spdlog::debug("BTN5 -> MENU");
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * btn_run_cb — GPIO23 RUN button
 * ───────────────────────────────────────────────────────────────────────── */
static void btn_run_cb(int /*gpio*/, int level, uint32_t /*tick*/)
{
    if (level != 0) return;
    auto *lp = g_link_ptr.load();
    if (!lp) { spdlog::warn("BTN23 -> ignored, Link not ready yet"); return; }

    auto state       = lp->captureAppSessionState();
    auto now         = lp->clock().micros();
    bool now_playing = !state.isPlaying();

    if (now_playing)
        state.setIsPlayingAndRequestBeatAtTime(true, now, 0.0, LINK_QUANTUM);
    else
        state.setIsPlaying(false, now);
    lp->commitAppSessionState(state);

    { std::lock_guard<std::mutex> lk(g_link_state.mtx); g_link_state.is_playing = now_playing; }
    g_redraw.store(true);
    spdlog::info("BTN23 -> transport {}", now_playing ? "ON" : "OFF");
}

/* ── Shared MIDI state ───────────────────────────────────────────────────── */
static MidiPort   g_midi_port;
static std::mutex g_midi_mtx;
static MidiState  g_midi;

/* ── Measure state ───────────────────────────────────────────────────────── */
static MeasureState g_measure;
static std::mutex   g_measure_mtx;
static MeasureState g_gpio_measure;
static std::mutex   g_gpio_measure_mtx;
static std::atomic<uint64_t> g_gpio_clk_count{0};
static MeasureLogger g_logger;

/* ── Pot constants ───────────────────────────────────────────────────────── */
static constexpr double POT_MENU_STEP = M_PI / 4.0;
static constexpr double POT_BPM_STEP  = M_PI / 4.0;
static constexpr double BPM_MIN       = 20.0;
static constexpr double BPM_MAX       = 300.0;

/* ─────────────────────────────────────────────────────────────────────────
 * init_logger
 * ───────────────────────────────────────────────────────────────────────── */
static void init_logger()
{
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_level(spdlog::level::debug);
    auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>("protosync.log", false);
    file->set_level(spdlog::level::debug);
    auto logger = std::make_shared<spdlog::logger>(
        "ps", spdlog::sinks_init_list{ console, file });
    logger->set_level(spdlog::level::debug);
    logger->set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");
    spdlog::set_default_logger(logger);
}

/* ─────────────────────────────────────────────────────────────────────────
 * input_thread_fn
 * ───────────────────────────────────────────────────────────────────────── */
static struct termios s_saved_term;
static void restore_terminal() { tcsetattr(STDIN_FILENO, TCSANOW, &s_saved_term); }

static void input_thread_fn()
{
    struct termios raw;
    tcgetattr(STDIN_FILENO, &s_saved_term);
    raw = s_saved_term;
    raw.c_lflag    &= ~(ICANON | ECHO);
    raw.c_cc[VMIN]  = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    spdlog::info("Input ready  (up/down scroll, SPACE open, ESC back, R reset measure)");

    while (g_running) {
        int c = getchar();
        if (c == EOF) break;
        if (c == 27) {
            int c2 = getchar();
            if (c2 == '[') {
                int c3 = getchar();
                if (c3 == 'A' && g_page == Page::MENU) {
                    int s = g_menu_sel.load();
                    g_menu_sel.store((s - 1 + NUM_PAGES) % NUM_PAGES);
                    g_redraw.store(true);
                } else if (c3 == 'B' && g_page == Page::MENU) {
                    int s = g_menu_sel.load();
                    g_menu_sel.store((s + 1) % NUM_PAGES);
                    g_redraw.store(true);
                }
            } else if (g_page != Page::MENU) {
                g_page.store(Page::MENU);
                g_redraw.store(true);
                spdlog::debug("Input -> MENU (ESC)");
            }
        } else if (c == ' ') {
            if (g_page == Page::MENU) {
                g_page.store(static_cast<Page>(g_menu_sel.load() + 1));
                g_redraw.store(true);
                spdlog::info("Open page {}", PAGE_NAMES[g_menu_sel.load()]);
            } else {
                g_page.store(Page::MENU);
                g_redraw.store(true);
                spdlog::debug("Input -> MENU (SPACE)");
            }
        } else if (c == 'r' || c == 'R') {
            {
                std::lock_guard<std::mutex> lk(g_measure_mtx);
                g_measure = MeasureState{};
                {
                    std::lock_guard<std::mutex> lk2(g_gpio_measure_mtx);
                    g_gpio_measure = MeasureState{};
                }
            }
            g_redraw.store(true);
            spdlog::info("Measure stats reset");
        }
    }
    restore_terminal();
    spdlog::debug("Input thread exiting");
}

/* ─────────────────────────────────────────────────────────────────────────
 * link_thread_fn
 * ───────────────────────────────────────────────────────────────────────── */
static void link_thread_fn()
{
    ableton::Link link(120.0);
    link.enableStartStopSync(true);

    link.setTempoCallback([](double bpm) {
        std::lock_guard<std::mutex> lk(g_link_state.mtx);
        g_link_state.bpm = bpm;
        spdlog::debug("Link tempo -> {:.2f} bpm", bpm);
    });
    link.setNumPeersCallback([](std::size_t n) {
        std::lock_guard<std::mutex> lk(g_link_state.mtx);
        g_link_state.peers = static_cast<uint32_t>(n);
        spdlog::info("Link peers -> {}", n);
    });
    link.setStartStopCallback([](bool playing) {
        { std::lock_guard<std::mutex> lk(g_link_state.mtx); g_link_state.is_playing = playing; }
        g_redraw.store(true);
        spdlog::info("Link transport -> {}", playing ? "PLAY" : "STOP");
    });

    link.enable(true);
    spdlog::info("Link enabled");

    {
        auto state = link.captureAppSessionState();
        state.setIsPlayingAndRequestBeatAtTime(true, link.clock().micros(), 0.0, LINK_QUANTUM);
        link.commitAppSessionState(state);
        spdlog::info("Link transport started");
    }

    g_link_ptr.store(&link);

    while (g_running) {
        auto   state     = link.captureAppSessionState();
        auto   now       = link.clock().micros();
        double beat      = state.beatAtTime(now, LINK_QUANTUM);
        double phase     = state.phaseAtTime(now, LINK_QUANTUM);
        int    bar       = static_cast<int>(beat / LINK_QUANTUM);
        auto   nbt       = state.timeAtBeat((bar + 1) * LINK_QUANTUM, LINK_QUANTUM);
        double ms_to_bar = static_cast<double>((nbt - now).count()) / 1000.0;
        {
            std::lock_guard<std::mutex> lk(g_link_state.mtx);
            g_link_state.beat      = beat;
            g_link_state.phase     = phase;
            g_link_state.bar       = bar;
            g_link_state.ms_to_bar = ms_to_bar;
        }
        usleep(1000);
    }

    g_link_ptr.store(nullptr);
    spdlog::debug("Link thread exiting");
}

/* ─────────────────────────────────────────────────────────────────────────
 * clock_thread_fn — Link-anchored GPIO beat clock          [CHANGE 1]
 *
 * Each pulse is phase-locked to the Link beat grid via timeAtBeat(),
 * eliminating the long-term drift of the previous usleep()-based approach.
 *
 * Pulse width is adaptive: min(CLOCK_PULSE_US, interval_us * 2/5)
 * so duty cycle never exceeds 40% — prevents pulse collision at high
 * PPQN + high BPM (e.g. 96 PPQN @ 300 BPM gives interval ~2 083 us).
 * ───────────────────────────────────────────────────────────────────────── */
static void clock_thread_fn()
{
    sched_param sp{};
    sp.sched_priority = 70;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        spdlog::warn("Clock thread: SCHED_FIFO failed (run as root?)");

    spdlog::info("Clock thread started -- {}PPQN GPIO{}, bar GPIO{}, run GPIO{}",
                 PPQN, GPIO_BEAT_CLK, GPIO_BAR_CLK, GPIO_RUN);

    if (!g_gpio_ok.load()) {
        spdlog::warn("Clock thread: pigpio not available, clock output disabled");
        while (g_running) usleep(50000);
        return;
    }

    uint32_t ppqn_count = 0;

    while (g_running) {

        auto *lp = g_link_ptr.load();
        bool  playing;
        { std::lock_guard<std::mutex> lk(g_link_state.mtx); playing = g_link_state.is_playing; }

        gpioWrite(GPIO_RUN, playing ? 1 : 0);

        if (!playing || !lp) {
            gpioWrite(GPIO_BEAT_CLK, 0);
            gpioWrite(GPIO_BAR_CLK,  0);
            ppqn_count = 0;
            usleep(20000);
            continue;
        }

        /* Link-anchored timing */
        auto   state     = lp->captureAppSessionState();
        auto   now       = lp->clock().micros();
        double bpm       = state.tempo();
        if (bpm < 1.0) bpm = 120.0;

        double beat_now  = state.beatAtTime(now, LINK_QUANTUM);
        double next_beat = (std::floor(beat_now * static_cast<double>(PPQN)) + 1.0)
                           / static_cast<double>(PPQN);
        auto next_tick_time = state.timeAtBeat(next_beat, LINK_QUANTUM);

        /* Adaptive pulse width -- max 40% duty cycle */
        uint32_t interval_us = static_cast<uint32_t>(
            60000000.0 / (bpm * static_cast<double>(PPQN)));
        uint32_t pulse_us = std::min(CLOCK_PULSE_US, interval_us * 2u / 5u);

        auto sleep_us = (next_tick_time - lp->clock().micros()).count();
        if (sleep_us > 0)
            usleep(static_cast<uint32_t>(sleep_us));

        /* Emit pulse */
        gpioWrite(GPIO_BEAT_CLK, 1);
        g_gpio_clk_count.fetch_add(1, std::memory_order_relaxed);

        if (ppqn_count % (static_cast<uint32_t>(PPQN) * 4u) == 0u)
            gpioWrite(GPIO_BAR_CLK, 1);

        usleep(pulse_us);

        gpioWrite(GPIO_BEAT_CLK, 0);
        gpioWrite(GPIO_BAR_CLK,  0);

        if (ppqn_count % (static_cast<uint32_t>(PPQN) * 4u) == 0u)
            spdlog::debug("CLK bar bpm={:.1f} interval={}us pulse={}us",
                          bpm, interval_us, pulse_us);
        ppqn_count++;
    }

    gpioWrite(GPIO_BEAT_CLK, 0);
    gpioWrite(GPIO_BAR_CLK,  0);
    gpioWrite(GPIO_RUN,      0);
    spdlog::debug("Clock thread exiting");
}

/* ─────────────────────────────────────────────────────────────────────────
 * midi_tx_thread_fn — Link-anchored MIDI clock master
 * ───────────────────────────────────────────────────────────────────────── */
static void midi_tx_thread_fn()
{
    sched_param sp{};
    sp.sched_priority = 65;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        spdlog::warn("MIDI TX thread: SCHED_FIFO failed");
    spdlog::info("MIDI TX thread started -- Link-anchored MIDI clock "
                 "(F8 @ 24 PPQN, FA/FC on transport edges)");

    bool last_playing = false;

    while (g_running) {
        auto *lp = g_link_ptr.load();
        if (!lp) { usleep(5000); continue; }

        auto   state   = lp->captureAppSessionState();
        auto   now     = lp->clock().micros();
        bool   playing = state.isPlaying();
        double bpm     = state.tempo();
        if (bpm < 1.0) bpm = 120.0;

        if (playing && !last_playing) {
            g_midi_port.sendStart();
            { std::lock_guard<std::mutex> lk(g_midi_mtx); g_midi.clk_running = true; }
            { std::lock_guard<std::mutex> lk(g_link_state.mtx); g_link_state.is_playing = true; }
            g_redraw.store(true);
            spdlog::info("MIDI TX: FA (Start) -- transport ON");
        }
        if (!playing && last_playing) {
            g_midi_port.sendStop();
            { std::lock_guard<std::mutex> lk(g_midi_mtx); g_midi.clk_running = false; }
            { std::lock_guard<std::mutex> lk(g_link_state.mtx); g_link_state.is_playing = false; }
            g_redraw.store(true);
            spdlog::info("MIDI TX: FC (Stop) -- transport OFF");
        }
        last_playing = playing;

        if (!playing) { usleep(50000); continue; }

        double beat_now       = state.beatAtTime(now, LINK_QUANTUM);
        double next_tick_beat = (std::floor(beat_now * static_cast<double>(MIDI_PPQN)) + 1.0)
                                / static_cast<double>(MIDI_PPQN);
        auto next_tick_time   = state.timeAtBeat(next_tick_beat, LINK_QUANTUM);
        auto sleep_us         = (next_tick_time - lp->clock().micros()).count();
        if (sleep_us > 0)
            usleep(static_cast<uint32_t>(sleep_us));

        g_midi_port.sendClock();
        { std::lock_guard<std::mutex> lk(g_midi_mtx); g_midi.clk_count++; }
    }

    if (last_playing) {
        g_midi_port.sendStop();
        spdlog::info("MIDI TX: FC (Stop) -- final shutdown");
    }
    spdlog::debug("MIDI TX thread exiting");
}

/* ─────────────────────────────────────────────────────────────────────────
 * midi_rx_thread_fn — MIDI input parser
 * ───────────────────────────────────────────────────────────────────────── */
static void midi_rx_thread_fn()
{
    spdlog::info("MIDI RX thread started (GPIO15 /dev/ttyAMA0)");
    while (g_running) {
        MidiState rx_tmp;
        { std::lock_guard<std::mutex> lk(g_midi_mtx); rx_tmp.rx_count = g_midi.rx_count; }
        bool got = g_midi_port.poll(rx_tmp);
        if (got) {
            std::lock_guard<std::mutex> lk(g_midi_mtx);
            g_midi.status             = rx_tmp.status;
            g_midi.data1              = rx_tmp.data1;
            g_midi.data2              = rx_tmp.data2;
            g_midi.channel            = rx_tmp.channel;
            g_midi.cc                 = rx_tmp.cc;
            g_midi.cc_val             = rx_tmp.cc_val;
            g_midi.rx_count           = rx_tmp.rx_count;
            g_midi.clock_tick         = rx_tmp.clock_tick;
            g_midi.transport_start    = rx_tmp.transport_start;
            g_midi.transport_stop     = rx_tmp.transport_stop;
            g_midi.transport_continue = rx_tmp.transport_continue;
            g_redraw.store(true);
        }
        usleep(1000);
    }
    spdlog::debug("MIDI RX thread exiting");
}

/* ─────────────────────────────────────────────────────────────────────────
 * measure_thread_fn — MIDI timing diagnostics (jitter + sync error)
 * ───────────────────────────────────────────────────────────────────────── */
static void measure_thread_fn()
{
    sched_param sp{};
    sp.sched_priority = 60;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        spdlog::warn("Measure thread: SCHED_FIFO failed (run as root?)");
    spdlog::info("Measure thread started");

    constexpr int WINDOW = MeasureState::SPARK_N;
    std::array<float, WINDOW> win = {};
    int  win_idx   = 0;
    int  win_count = 0;

    uint64_t last_tick_ns  = 0;
    uint64_t last_clk_seen = 0;

    while (g_running) {

        uint64_t cur_clk;
        double   bpm;
        bool     playing;
        { std::lock_guard<std::mutex> lk(g_midi_mtx); cur_clk = g_midi.clk_count; playing = g_midi.clk_running; }
        { std::lock_guard<std::mutex> lk(g_link_state.mtx); bpm = g_link_state.bpm; }
        if (bpm < 1.0) bpm = 120.0;

        if (!playing) {
            last_tick_ns  = 0;
            last_clk_seen = 0;
            win_count     = 0;
            win_idx       = 0;
            usleep(10000);
            continue;
        }

        if (cur_clk != last_clk_seen) {
            uint64_t now_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());

            if (last_tick_ns != 0 && cur_clk == last_clk_seen + 1) {

                float actual_us   = static_cast<float>(now_ns - last_tick_ns) / 1000.0f;
                float expected_us = static_cast<float>(60000000.0 / (bpm * MIDI_PPQN));
                float jitter_us   = actual_us - expected_us;

                /* Rolling window stats */
                win[win_idx % WINDOW] = actual_us;
                win_idx++;
                if (win_count < WINDOW) win_count++;

                float mn = win[0], mx = win[0], sum = 0.0f;
                for (int i = 0; i < win_count; ++i) {
                    int   idx = (win_idx - win_count + i + WINDOW) % WINDOW;
                    float v   = win[idx];
                    sum += v;
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                }
                float mean = sum / static_cast<float>(win_count);

                float sq_sum = 0.0f;
                for (int i = 0; i < win_count; ++i) {
                    int   idx = (win_idx - win_count + i + WINDOW) % WINDOW;
                    float d   = win[idx] - mean;
                    sq_sum   += d * d;
                }
                float stddev = (win_count > 1)
                    ? sqrtf(sq_sum / static_cast<float>(win_count - 1)) : 0.0f;

                /* Sync error vs Link beat grid */
                double sync_err_us = 0.0;
                auto  *lp = g_link_ptr.load();
                if (lp) {
                    auto   state      = lp->captureAppSessionState();
                    auto   link_now   = lp->clock().micros();
                    double beat_now   = state.beatAtTime(link_now, LINK_QUANTUM);
                    double grid_beat  = std::round(beat_now * MIDI_PPQN)
                                        / static_cast<double>(MIDI_PPQN);
                    double us_per_tick = 60000000.0 / (bpm * static_cast<double>(MIDI_PPQN));
                    sync_err_us = (beat_now - grid_beat) * us_per_tick;
                }

                {
                    std::lock_guard<std::mutex> lk(g_measure_mtx);
                    g_measure.interval_mean_us   = mean;
                    g_measure.interval_min_us    = mn;
                    g_measure.interval_max_us    = mx;
                    g_measure.interval_stddev_us = stddev;
                    g_measure.jitter_last_us     = jitter_us;
                    g_measure.sync_error_us      = sync_err_us;
                    g_measure.sample_count++;
                    float abs_j = fabsf(jitter_us);
                    if (abs_j > g_measure.jitter_peak_us)
                        g_measure.jitter_peak_us = abs_j;
                    g_measure.spark[g_measure.spark_head] = jitter_us;
                    g_measure.spark_head =
                        (g_measure.spark_head + 1) % MeasureState::SPARK_N;
                }

                if (cur_clk % 4 == 0) g_redraw.store(true);
                if (cur_clk % 48 == 0)
                    spdlog::debug("MEASURE  mean={:.1f}us  s={:.1f}us  peak={:.1f}us"
                                  "  jitter={:+.1f}us  sync={:+.1f}us  n={}",
                                  mean, stddev, g_measure.jitter_peak_us,
                                  jitter_us, sync_err_us, g_measure.sample_count);
            }
            last_tick_ns  = now_ns;
            last_clk_seen = cur_clk;
        }
        usleep(500);
    }
    spdlog::debug("Measure thread exiting");
}

/* ─────────────────────────────────────────────────────────────────────────
 * gpio_measure_thread_fn — GPIO clock diagnostics          [CHANGE 2]
 *
 * sync_error_us is now computed against the Link beat grid (was 0.0).
 * GPIO clock is phase-locked to Link via timeAtBeat() so the measurement
 * is meaningful.
 * ───────────────────────────────────────────────────────────────────────── */
static void gpio_measure_thread_fn()
{
    sched_param sp{};
    sp.sched_priority = 58;
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        spdlog::warn("GPIO measure thread: SCHED_FIFO failed");
    spdlog::info("GPIO measure thread started -- {} PPQN GPIO{}", PPQN, GPIO_BEAT_CLK);

    constexpr int WINDOW = MeasureState::SPARK_N;
    std::array<float, WINDOW> win = {};
    int  win_idx   = 0;
    int  win_count = 0;

    uint64_t last_tick_ns  = 0;
    uint64_t last_clk_seen = 0;

    while (g_running) {

        uint64_t cur_clk = g_gpio_clk_count.load(std::memory_order_relaxed);
        double   bpm;
        bool     playing;
        { std::lock_guard<std::mutex> lk(g_link_state.mtx); bpm = g_link_state.bpm; playing = g_link_state.is_playing; }
        if (bpm < 1.0) bpm = 120.0;

        if (!playing) {
            last_tick_ns  = 0;
            last_clk_seen = 0;
            win_count     = 0;
            win_idx       = 0;
            usleep(10000);
            continue;
        }

        if (cur_clk != last_clk_seen) {
            uint64_t now_ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());

            if (last_tick_ns != 0 && cur_clk == last_clk_seen + 1) {

                float actual_us   = static_cast<float>(now_ns - last_tick_ns) / 1000.0f;
                float expected_us = static_cast<float>(
                    60000000.0 / (bpm * static_cast<double>(PPQN)));
                float jitter_us   = actual_us - expected_us;

                win[win_idx % WINDOW] = actual_us;
                win_idx++;
                if (win_count < WINDOW) win_count++;

                float mn = win[0], mx = win[0], sum = 0.0f;
                for (int i = 0; i < win_count; ++i) {
                    int   idx = (win_idx - win_count + i + WINDOW) % WINDOW;
                    float v   = win[idx];
                    sum += v;
                    if (v < mn) mn = v;
                    if (v > mx) mx = v;
                }
                float mean = sum / static_cast<float>(win_count);

                float sq_sum = 0.0f;
                for (int i = 0; i < win_count; ++i) {
                    int   idx = (win_idx - win_count + i + WINDOW) % WINDOW;
                    float d   = win[idx] - mean;
                    sq_sum   += d * d;
                }
                float stddev = (win_count > 1)
                    ? sqrtf(sq_sum / static_cast<float>(win_count - 1)) : 0.0f;

                /* Sync error vs Link beat grid (now computable -- GPIO is Link-anchored) */
                double sync_err_us = 0.0;
                auto  *lp = g_link_ptr.load();
                if (lp) {
                    auto   state       = lp->captureAppSessionState();
                    auto   link_now    = lp->clock().micros();
                    double beat_now    = state.beatAtTime(link_now, LINK_QUANTUM);
                    double grid_beat   = std::round(beat_now * static_cast<double>(PPQN))
                                         / static_cast<double>(PPQN);
                    double us_per_tick = 60000000.0 / (bpm * static_cast<double>(PPQN));
                    sync_err_us = (beat_now - grid_beat) * us_per_tick;
                }

                {
                    std::lock_guard<std::mutex> lk(g_gpio_measure_mtx);
                    g_gpio_measure.interval_mean_us   = mean;
                    g_gpio_measure.interval_min_us    = mn;
                    g_gpio_measure.interval_max_us    = mx;
                    g_gpio_measure.interval_stddev_us = stddev;
                    g_gpio_measure.jitter_last_us     = jitter_us;
                    g_gpio_measure.sync_error_us      = sync_err_us;
                    g_gpio_measure.sample_count++;
                    float abs_j = fabsf(jitter_us);
                    if (abs_j > g_gpio_measure.jitter_peak_us)
                        g_gpio_measure.jitter_peak_us = abs_j;
                    g_gpio_measure.spark[g_gpio_measure.spark_head] = jitter_us;
                    g_gpio_measure.spark_head =
                        (g_gpio_measure.spark_head + 1) % MeasureState::SPARK_N;
                }

                if (cur_clk % 4 == 0) g_redraw.store(true);
                if (cur_clk % 16 == 0)
                    spdlog::debug("GPIO CLK  mean={:.1f}us  s={:.1f}us  peak={:.1f}us"
                                  "  jitter={:+.1f}us  sync={:+.1f}us  n={}",
                                  mean, stddev, g_gpio_measure.jitter_peak_us,
                                  jitter_us, sync_err_us, g_gpio_measure.sample_count);
            }
            last_tick_ns  = now_ns;
            last_clk_seen = cur_clk;
        }
        usleep(500);
    }
    spdlog::debug("GPIO measure thread exiting");
}

/* ─────────────────────────────────────────────────────────────────────────
 * logger_thread_fn — 1 Hz CSV logger
 * ───────────────────────────────────────────────────────────────────────── */
static void logger_thread_fn()
{
    spdlog::info("Logger thread started -- 1 Hz CSV logging to {}", g_logger.path());
    while (g_running) {
        for (int i = 0; i < 20 && g_running; ++i) usleep(50000);
        double bpm; bool playing;
        { std::lock_guard<std::mutex> lk(g_link_state.mtx); bpm = g_link_state.bpm; playing = g_link_state.is_playing; }
        if (!playing) continue;
        MeasureState midi_snap, gpio_snap;
        { std::lock_guard<std::mutex> lk(g_measure_mtx);      midi_snap = g_measure; }
        { std::lock_guard<std::mutex> lk(g_gpio_measure_mtx); gpio_snap = g_gpio_measure; }
        g_logger.log(bpm, midi_snap, gpio_snap);
    }
    spdlog::debug("Logger thread exiting");
}

/* ─────────────────────────────────────────────────────────────────────────
 * main
 * ───────────────────────────────────────────────────────────────────────── */
int main()
{
    init_logger();
    spdlog::info("Starting...");
    if (!g_logger.open("."))
        spdlog::warn("Logger: failed to open CSV -- data will not be saved");

    if (DEV_ModuleInit() != 0) { spdlog::error("DEV_ModuleInit failed"); return 1; }
    LCD_2IN_Init();
    LCD_2IN_Clear(BLACK);
    LCD_SetBacklight(50);
    spdlog::info("LCD ready");

    if (gpioInitialise() < 0) {
        spdlog::error("pigpio init failed -- run as root? clock output disabled");
    } else {
        g_gpio_ok.store(true);
        gpioSetMode(GPIO_BEAT_CLK, PI_OUTPUT);
        gpioSetMode(GPIO_BAR_CLK,  PI_OUTPUT);
        gpioSetMode(GPIO_RUN,      PI_OUTPUT);
        gpioWrite(GPIO_BEAT_CLK, 0);
        gpioWrite(GPIO_BAR_CLK,  0);
        gpioWrite(GPIO_RUN,      0);
        gpioSetPullUpDown(GPIO_BEAT_CLK, PI_PUD_OFF);
        gpioSetPullUpDown(GPIO_BAR_CLK,  PI_PUD_OFF);
        gpioSetPullUpDown(GPIO_RUN,      PI_PUD_OFF);
        gpioSetPad(0, 16);
        spdlog::info("pigpio OK -- GPIO{} GPIO{} GPIO{}",
                     GPIO_BEAT_CLK, GPIO_BAR_CLK, GPIO_RUN);

        gpioSetMode(GPIO_BTN_ENTER, PI_INPUT);
        gpioSetPullUpDown(GPIO_BTN_ENTER, PI_PUD_UP);
        gpioGlitchFilter(GPIO_BTN_ENTER, 10000);
        gpioSetAlertFunc(GPIO_BTN_ENTER, btn_enter_cb);
        spdlog::info("BTN ENTER on GPIO{} (active low, PU, 10ms debounce)", GPIO_BTN_ENTER);

        gpioSetMode(GPIO_BTN_RUN, PI_INPUT);
        gpioSetPullUpDown(GPIO_BTN_RUN, PI_PUD_UP);
        gpioGlitchFilter(GPIO_BTN_RUN, 10000);
        gpioSetAlertFunc(GPIO_BTN_RUN, btn_run_cb);
        spdlog::info("BTN RUN   on GPIO{} (active low, PU, 10ms debounce)", GPIO_BTN_RUN);
    }

    if (!g_midi_port.open("/dev/ttyAMA0")) {
        spdlog::warn("MIDI port unavailable -- TX/RX threads will idle");
        spdlog::warn("Hint: add 'enable_uart=1' and 'dtoverlay=disable-bt' "
                     "to /boot/firmware/config.txt, then reboot");
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGTSTP, on_signal);

    ProtoSyncUI ui;
    if (!ui.init()) {
        spdlog::error("UI init failed (OOM)");
        g_midi_port.close(); gpioTerminate(); DEV_ModuleExit(); return 1;
    }

    Ads1050 adc;
    bool has_adc = adc.init();
    if (!has_adc) spdlog::warn("ADS1050 init failed -- continuing without ADC");
    else          spdlog::info("ADC OK at 0x{:02x}", adc.address);

    std::thread clock_thr        (clock_thread_fn);
    std::thread link_thr         (link_thread_fn);
    std::thread input_thr        (input_thread_fn);
    std::thread midi_tx_thr      (midi_tx_thread_fn);
    std::thread midi_rx_thr      (midi_rx_thread_fn);
    std::thread measure_thr      (measure_thread_fn);
    std::thread gpio_measure_thr (gpio_measure_thread_fn);
    std::thread logger_thr       (logger_thread_fn);

    spdlog::info("Running. Press Ctrl-C to quit.");

    double   last_bpm     = -1.0;
    uint32_t last_peers   = UINT32_MAX;
    int      last_beat    = -1;
    bool     last_playing = false;
    int      last_bar     = -1;
    double   last_phase   = -1.0;
    Page     last_page    = static_cast<Page>(-1);
    int      last_sel     = -1;

    double last_angle      = 0.0;
    double accum_angle     = 0.0;
    int    last_menu_steps = 0;
    int    last_link_steps = 0;

    std::array<uint16_t, 2> ch_min = { 0, 0 };
    std::array<uint16_t, 2> ch_max = { 0, 0 };
    bool cal_seeded = false;

    std::array<std::optional<uint16_t>, 4> last_adc = {
        std::nullopt, std::nullopt, std::nullopt, std::nullopt };

    uint32_t last_sample_count = UINT32_MAX;
    int frame = 0;

    while (g_running) {

        Page cur_page = g_page.load();
        int  cur_sel  = g_menu_sel.load();

        double bpm, beat, phase, ms_to_bar;
        uint32_t peers;
        bool is_playing;
        int  bar;
        {
            std::lock_guard<std::mutex> lk(g_link_state.mtx);
            bpm=g_link_state.bpm; beat=g_link_state.beat; phase=g_link_state.phase;
            peers=g_link_state.peers; is_playing=g_link_state.is_playing;
            bar=g_link_state.bar; ms_to_bar=g_link_state.ms_to_bar;
        }
        int beat_idx = static_cast<int>(fabs(beat)) % 4;

        auto adc_vals = has_adc ? adc.readAllChannels() : last_adc;
        bool adc_changed = false;
        if (has_adc)
            for (int i = 0; i < 4; ++i)
                if (adc_vals[i] != last_adc[i]) { adc_changed = true; break; }

        double angle = 0.0;
        int    dial_pos = 0;

        if (adc_vals[0].has_value() && adc_vals[1].has_value()) {
            uint16_t a = adc_vals[0].value(), b = adc_vals[1].value();
            if (!cal_seeded) {
                ch_min[0]=ch_max[0]=a; ch_min[1]=ch_max[1]=b; cal_seeded=true;
            }
            ch_min[0]=std::min(ch_min[0],a); ch_max[0]=std::max(ch_max[0],a);
            ch_min[1]=std::min(ch_min[1],b); ch_max[1]=std::max(ch_max[1],b);

            double range_a = ch_max[0]-ch_min[0];
            double range_b = ch_max[1]-ch_min[1];

            if (range_a >= 50.0 && range_b >= 50.0) {
                double fa = ((static_cast<double>(a)-ch_min[0])/range_a)*2.0-1.0;
                double fb = ((static_cast<double>(b)-ch_min[1])/range_b)*2.0-1.0;
                angle = atan2(fa, fb);

                double delta = angle - last_angle;
                if (delta >  M_PI) delta -= 2.0*M_PI;
                if (delta < -M_PI) delta += 2.0*M_PI;
                accum_angle += delta;
                last_angle   = angle;

                if (cur_page == Page::MENU) {
                    int cur_steps = static_cast<int>(floor(accum_angle/POT_MENU_STEP));
                    int ds = cur_steps - last_menu_steps;
                    if (ds != 0) {
                        int s = g_menu_sel.load();
                        s = ((s+ds)%NUM_PAGES+NUM_PAGES)%NUM_PAGES;
                        g_menu_sel.store(s); g_redraw.store(true);
                        last_menu_steps = cur_steps;
                        spdlog::debug("Pot scroll steps={} sel={}", ds, s);
                    }
                } else if (cur_page == Page::LINK) {
                    int cur_steps = static_cast<int>(floor(accum_angle/POT_BPM_STEP));
                    int ds = cur_steps - last_link_steps;
                    if (ds != 0) {
                        auto *lp = g_link_ptr.load();
                        if (lp) {
                            double cur_bpm;
                            { std::lock_guard<std::mutex> lk(g_link_state.mtx); cur_bpm=g_link_state.bpm; }
                            double new_bpm = std::max(BPM_MIN, std::min(BPM_MAX,
                                                cur_bpm + static_cast<double>(ds)));
                            auto state = lp->captureAppSessionState();
                            state.setTempo(new_bpm, lp->clock().micros());
                            lp->commitAppSessionState(state);
                            g_redraw.store(true);
                            spdlog::debug("Pot BPM -> {:.1f}", new_bpm);
                        }
                        last_link_steps = cur_steps;
                    }
                }

                dial_pos = static_cast<int>((angle+M_PI)/(2.0*M_PI)*127.0+0.5);
                dial_pos = dial_pos < 0 ? 0 : (dial_pos > 127 ? 127 : dial_pos);
            }
        }

        bool force = g_redraw.exchange(false) || (frame % 50 == 0);
        cur_sel = g_menu_sel.load();

        if (cur_page == Page::MENU) {
            if (force || cur_page != last_page || cur_sel != last_sel) {
                ui.drawMenu(cur_sel);
                last_sel = cur_sel;
            }
        } else if (cur_page == Page::LINK) {
            if (force || cur_page != last_page ||
                fabs(bpm-last_bpm) > 0.01 || peers != last_peers ||
                beat_idx != last_beat || is_playing != last_playing ||
                bar != last_bar || fabs(phase-last_phase) > 0.04) {
                ui.drawLink(bpm, beat, phase, peers, is_playing, bar, ms_to_bar);
                last_bpm=bpm; last_peers=peers; last_beat=beat_idx;
                last_playing=is_playing; last_bar=bar; last_phase=phase;
            }
        } else if (cur_page == Page::ADC) {
            if (force || cur_page != last_page || adc_changed) {
                ui.drawAdc(adc_vals, angle, dial_pos, accum_angle);
                last_adc = adc_vals;
            }
        } else if (cur_page == Page::MIDI) {
            if (force || cur_page != last_page) {
                MidiState midi_snap;
                { std::lock_guard<std::mutex> lk(g_midi_mtx); midi_snap=g_midi; }
                ui.drawMidi(midi_snap);
            }
        } else if (cur_page == Page::MEASURE) {
            MeasureState snap; uint32_t snap_count;
            { std::lock_guard<std::mutex> lk(g_measure_mtx);
              snap=g_measure; snap_count=g_measure.sample_count; }
            if (force || cur_page != last_page || snap_count != last_sample_count) {
                ui.drawMeasure(snap);
                last_sample_count = snap_count;
            }
        } else if (cur_page == Page::GPIOCLK) {
            MeasureState gpio_snap; uint32_t gpio_snap_count;
            { std::lock_guard<std::mutex> lk(g_gpio_measure_mtx);
              gpio_snap=g_gpio_measure; gpio_snap_count=g_gpio_measure.sample_count; }
            if (force || cur_page != last_page || gpio_snap_count != last_sample_count) {
                ui.drawGpioMeasure(gpio_snap);
                last_sample_count = gpio_snap_count;
            }
        }

        last_page = cur_page;
        frame++;

        /* Auto-shutdown after 10 000 render frames */
        if (frame >= 10000) {
            spdlog::info("Frame limit reached ({} frames, ~{:.0f}s) -- shutting down",
                         frame, frame * 20000.0 / 1e6);
            g_running = 0;
        }

        usleep(20000);
    }

    spdlog::info("Shutting down...");

    if (g_gpio_ok.load()) {
        gpioSetAlertFunc(GPIO_BTN_ENTER, nullptr);
        gpioSetAlertFunc(GPIO_BTN_RUN,   nullptr);
    }

    input_thr.join();
    midi_rx_thr.join();
    midi_tx_thr.join();
    logger_thr.join();
    g_logger.close();
    gpio_measure_thr.join();
    measure_thr.join();
    link_thr.join();
    clock_thr.join();

    g_midi_port.close();

    if (g_gpio_ok.load()) {
        gpioWrite(GPIO_BEAT_CLK, 0);
        gpioWrite(GPIO_BAR_CLK,  0);
        gpioWrite(GPIO_RUN,      0);
        gpioSetPullUpDown(GPIO_BEAT_CLK, PI_PUD_DOWN);
        gpioSetPullUpDown(GPIO_BAR_CLK,  PI_PUD_DOWN);
        gpioSetPullUpDown(GPIO_RUN,      PI_PUD_DOWN);
        gpioTerminate();
    }

    ui.shutdown();
    DEV_ModuleExit();
    spdlog::shutdown();
    return 0;
}