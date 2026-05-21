/**
 * ui.hpp
 * ProtoSync LCD UI — all types, constants and class declaration.
 */

#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "midi.hpp"

extern "C" {
#include "LCD_2inch.h"
#include "DEV_Config.h"
#include "GUI_Paint.h"
#include "GUI_BMP.h"
#include "fonts.h"
}

/* ─────────────────────────────────────────────────────────────────────────
 * Display geometry
 * ───────────────────────────────────────────────────────────────────────── */
static constexpr int DISP_W = 240;
static constexpr int DISP_H = 320;

/* ─────────────────────────────────────────────────────────────────────────
 * RGB-565 colour palette
 * ───────────────────────────────────────────────────────────────────────── */
#define RGB565(r, g, b) \
    static_cast<UWORD>( \
        (static_cast<uint16_t>((r) & 0xF8u) << 8) | \
        (static_cast<uint16_t>((g) & 0xFCu) << 3) | \
        (static_cast<uint16_t>((b) & 0xF8u) >> 3) )

/* Backgrounds */
static constexpr UWORD COL_BG       = RGB565( 10,  12,  18);
static constexpr UWORD COL_MENU_HDR = RGB565( 18,  28,  56);

/* Text / labels */
static constexpr UWORD COL_TITLE    = RGB565(255, 255, 255);
static constexpr UWORD COL_LABEL    = RGB565(110, 120, 140);

/* Selection highlight */
static constexpr UWORD COL_SEL_BG   = RGB565( 30,  80, 200);
static constexpr UWORD COL_SEL_FG   = RGB565(255, 255, 255);
static constexpr UWORD COL_ITEM_FG  = RGB565(200, 210, 230);

/* Data colours */
static constexpr UWORD COL_BPM      = RGB565(  0, 220, 180);
static constexpr UWORD COL_PEERS    = RGB565(  0, 180, 255);
static constexpr UWORD COL_BEAT_ON  = RGB565( 80, 255,  80);
static constexpr UWORD COL_BEAT_OFF = RGB565( 28,  32,  40);

/* Traffic-light indicators */
static constexpr UWORD COL_GOOD     = RGB565( 60, 230,  80);  /* < 500 µs  */
static constexpr UWORD COL_WARN     = RGB565(255, 200,   0);  /* < 2000 µs */
static constexpr UWORD COL_ERR      = RGB565(255,  50,  50);  /* >= 2000 µs */

/* ─────────────────────────────────────────────────────────────────────────
 * Pages
 * ───────────────────────────────────────────────────────────────────────── */
enum class Page : int {
    MENU     = 0,
    LINK     = 1,
    ADC      = 2,
    MIDI     = 3,
    MEASURE  = 4,
    GPIOCLK  = 5,
};

static constexpr int NUM_PAGES = 5;

static constexpr const char *PAGE_NAMES[NUM_PAGES] = {
    "LINK",
    "ADC",
    "MIDI",
    "MEASURE",
    "GPIO CLK",
};

/* ─────────────────────────────────────────────────────────────────────────
 * MeasureState
 * Generic timing diagnostics block — used for both MIDI clock and GPIO clock
 * measurements.  Written by a dedicated measure thread, snapshot-copied by
 * the render loop under an external mutex (no mutex member → trivially
 * copyable).
 *
 * Fields marked "sync_error_us" are only meaningful for the MIDI clock
 * variant (where Link phase can be compared); for GPIO they are left 0.
 * ───────────────────────────────────────────────────────────────────────── */
struct MeasureState {
    // --- istniejące pola (bez zmian) ---
    float    interval_mean_us   = 0.0f;
    float    interval_min_us    = 0.0f;
    float    interval_max_us    = 0.0f;
    float    interval_stddev_us = 0.0f;
    float    jitter_last_us     = 0.0f;
    float    jitter_peak_us     = 0.0f;
    double   sync_error_us      = 0.0;
    uint32_t sample_count       = 0;
 
    static constexpr int SPARK_N = 64;
    float    spark[SPARK_N]     = {};
    int      spark_head         = 0;
 
};


/* ─────────────────────────────────────────────────────────────────────────
 * ProtoSyncUI
 * ───────────────────────────────────────────────────────────────────────── */
class ProtoSyncUI {
public:
    bool init();
    void shutdown();

    void drawMenu(int sel);

    void drawLink(double   bpm,
                  double   beat,
                  double   phase,
                  uint32_t peers,
                  bool     is_playing,
                  int      bar,
                  double   ms_to_bar);

    void drawAdc(const std::array<std::optional<uint16_t>, 4> &adc,
                 double angle,
                 int    dial_pos,
                 double accum_angle);

    void drawMidi(const MidiState &m);

    /* MIDI 24-PPQN timing diagnostics */
    void drawMeasure(const MeasureState &ms);

    /* GPIO 4-PPQN clock timing diagnostics */
    void drawGpioMeasure(const MeasureState &ms);

private:
    UWORD *buf_ = nullptr;

    void drawSectionHeader(int y, const char *label);
    void drawSparkline(int x, int y, int w, int h, const MeasureState &ms);

    /* Shared implementation for both measure pages — title selects the
     * header text and whether the LINK SYNC ERR section is shown.       */
    void drawMeasurePage(const char *title,
                         const MeasureState &ms,
                         bool show_sync_err);
};