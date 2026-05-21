/**
 * ui.cpp
 * ProtoSyncUI implementation — all LCD drawing logic.
 *
 * Display: Waveshare 2-inch IPS (240 × 320, RGB-565).
 * Driver:  LCD_2IN / Paint library (Waveshare C examples).
 *
 * Every draw* function:
 *   1. Selects the back-buffer  (Paint_SelectImage)
 *   2. Clears it to COL_BG
 *   3. Renders all UI elements into the buffer
 *   4. Flushes to hardware      (LCD_2IN_Display)
 *
 * No partial updates — full-frame blit on every call.  The render loop in
 * main.cpp throttles calls to ~50 fps and only calls when data changes.
 */

#include "ui.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

/* ─────────────────────────────────────────────────────────────────────────
 * ProtoSyncUI::init
 * Allocates the 16-bit (RGB-565) pixel back-buffer used by the Paint API.
 * Returns false on OOM (caller must abort).
 * ───────────────────────────────────────────────────────────────────────── */
bool ProtoSyncUI::init()
{
    UDOUBLE sz = static_cast<UDOUBLE>(DISP_W) * DISP_H * 2;
    buf_ = static_cast<UWORD *>(malloc(sz));
    if (!buf_) return false;

    Paint_NewImage(buf_, DISP_W, DISP_H, 0, BLACK, 16);
    Paint_Clear(BLACK);
    return true;
}

/* ─────────────────────────────────────────────────────────────────────────
 * ProtoSyncUI::shutdown
 * Blanks the display and releases the pixel buffer.
 * ───────────────────────────────────────────────────────────────────────── */
void ProtoSyncUI::shutdown()
{
    if (buf_) {
        LCD_2IN_Clear(BLACK);
        free(buf_);
        buf_ = nullptr;
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * ProtoSyncUI::drawMenu
 *
 * Layout (240 × 320):
 *   0.. 36   dark header bar  "MENU"
 *  36.. 56   navigation hint line
 *  56.. 62   hairline separator
 *  72..end   menu items (36 px per item, highlight bar on selection)
 * ───────────────────────────────────────────────────────────────────────── */
void ProtoSyncUI::drawMenu(int sel)
{
    Paint_SelectImage(buf_);
    Paint_Clear(COL_BG);

    Paint_DrawRectangle(0, 0, DISP_W, 36, COL_MENU_HDR, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(12, 9,  "MENU",                       &Font20, COL_MENU_HDR, COL_TITLE);
    Paint_DrawString_EN(12, 44, "UP/DN  SPACE=open  ESC=back", &Font12, COL_BG,      COL_LABEL);
    Paint_DrawLine(12, 62, DISP_W - 12, 62, COL_LABEL, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    for (int i = 0; i < NUM_PAGES; ++i) {
        int y = 72 + i * 36;
        if (i == sel) {
            Paint_DrawRectangle(8, y - 2, DISP_W - 8, y + 28,
                                COL_SEL_BG, DOT_PIXEL_1X1, DRAW_FILL_FULL);
            Paint_DrawString_EN(16,          y + 6, PAGE_NAMES[i], &Font16, COL_SEL_BG, COL_SEL_FG);
            Paint_DrawString_EN(DISP_W - 28, y + 6, ">",           &Font16, COL_SEL_BG, COL_SEL_FG);
        } else {
            Paint_DrawString_EN(16, y + 6, PAGE_NAMES[i], &Font16, COL_BG, COL_ITEM_FG);
        }
    }

    LCD_2IN_Display(reinterpret_cast<UBYTE *>(buf_));
}

/* ─────────────────────────────────────────────────────────────────────────
 * ProtoSyncUI::drawLink
 *
 * Layout (240 × 320):
 *   0.. 36   header bar  "ABLETON LINK"  + peer/play status dots
 *  36..112   BPM (large) + min/max labels
 * 112..162   Bar number + ms-to-next-bar countdown
 * 162..224   Beat dot indicators (4 dots)
 * 224..242   Phase progress bar
 * 242..300   Peer count
 * ───────────────────────────────────────────────────────────────────────── */
void ProtoSyncUI::drawLink(double   bpm,
                            double   beat,
                            double   phase,
                            uint32_t peers,
                            bool     is_playing,
                            int      bar,
                            double   ms_to_bar)
{
    Paint_SelectImage(buf_);
    Paint_Clear(COL_BG);

    /* Header */
    Paint_DrawRectangle(0, 0, DISP_W, 36, COL_MENU_HDR, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(12, 9, "ABLETON LINK", &Font20, COL_MENU_HDR, COL_TITLE);
    Paint_DrawString_EN(DISP_W - 52, 12, "ESC", &Font12, COL_MENU_HDR, COL_LABEL);

    /* Status dots: peers (cyan) and play/stop (green/red) */
    UWORD peer_col = (peers > 0) ? COL_PEERS : COL_LABEL;
    Paint_DrawCircle(DISP_W - 64, 18, 7, peer_col, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    if (is_playing)
        Paint_DrawCircle(DISP_W - 82, 18, 7, COL_BPM,    DOT_PIXEL_1X1, DRAW_FILL_FULL);
    else
        Paint_DrawCircle(DISP_W - 82, 18, 7, COL_BEAT_ON, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);

    /* BPM */
    Paint_DrawString_EN(12, 48, "TEMPO", &Font16, COL_BG, COL_LABEL);
    char bpm_str[16];
    snprintf(bpm_str, sizeof(bpm_str), "%6.2f", bpm);
    Paint_DrawString_EN(12, 68, bpm_str, &Font24, COL_BG, COL_BPM);
    Paint_DrawString_EN(12 + 6 * 24, 78, "BPM", &Font16, COL_BG, COL_LABEL);

    Paint_DrawLine(12, 112, DISP_W - 12, 112, COL_LABEL, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    /* Bar / next-bar countdown */
    Paint_DrawString_EN(12, 120, "BAR",  &Font12, COL_BG, COL_LABEL);
    Paint_DrawString_EN(80, 120, "NEXT", &Font12, COL_BG, COL_LABEL);

    char bar_str[12];
    snprintf(bar_str, sizeof(bar_str), "%d", bar + 1);
    Paint_DrawString_EN(12, 134, bar_str, &Font20, COL_BG, COL_PEERS);

    char ms_str[16];
    if (ms_to_bar >= 0) snprintf(ms_str, sizeof(ms_str), "%.0fms", ms_to_bar);
    else                snprintf(ms_str, sizeof(ms_str), "---");
    Paint_DrawString_EN(80, 134, ms_str, &Font20, COL_BG, COL_LABEL);

    Paint_DrawLine(12, 162, DISP_W - 12, 162, COL_LABEL, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    /* Beat dots */
    Paint_DrawString_EN(12, 170, "BEAT", &Font16, COL_BG, COL_LABEL);
    static constexpr double QUANTUM = 4.0;
    int beat_idx = static_cast<int>(fabs(beat)) % static_cast<int>(QUANTUM);
    for (int i = 0; i < 4; ++i) {
        UWORD col = (i == beat_idx) ? COL_BEAT_ON : COL_BEAT_OFF;
        Paint_DrawCircle(12 + i * 52 + 18, 204, 14, col, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    }

    /* Phase progress bar */
    static constexpr int BAR_X = 12;
    static constexpr int BAR_Y = 224;
    static constexpr int BAR_W = DISP_W - 24;
    static constexpr int BAR_H = 8;
    Paint_DrawRectangle(BAR_X, BAR_Y, BAR_X + BAR_W, BAR_Y + BAR_H,
                        COL_BEAT_OFF, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    int fill_w = static_cast<int>((phase / QUANTUM) * BAR_W);
    if (fill_w > 0)
        Paint_DrawRectangle(BAR_X, BAR_Y, BAR_X + fill_w, BAR_Y + BAR_H,
                            COL_BPM, DOT_PIXEL_1X1, DRAW_FILL_FULL);

    Paint_DrawLine(12, 242, DISP_W - 12, 242, COL_LABEL, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    /* Peer count */
    Paint_DrawString_EN(12, 252, "PEERS", &Font16, COL_BG, COL_LABEL);
    char peer_str[8];
    snprintf(peer_str, sizeof(peer_str), "%u", peers);
    Paint_DrawString_EN(12, 272, peer_str, &Font24, COL_BG, COL_PEERS);

    LCD_2IN_Display(reinterpret_cast<UBYTE *>(buf_));
}

/* ─────────────────────────────────────────────────────────────────────────
 * ProtoSyncUI::drawAdc
 *
 * Layout (240 × 320):
 *   0.. 36   header bar  "ADC"
 *  36..240   quadrature dial (circle + needle)
 * 240..258   angle in degrees
 * 298..320   raw CH0–CH3 values (2 × 2 grid)
 * ───────────────────────────────────────────────────────────────────────── */
void ProtoSyncUI::drawAdc(const std::array<std::optional<uint16_t>, 4> &adc,
                           double angle,
                           int    dial_pos,
                           double /*accum_angle*/)
{
    Paint_SelectImage(buf_);
    Paint_Clear(COL_BG);

    Paint_DrawRectangle(0, 0, DISP_W, 36, COL_MENU_HDR, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(12, 9, "ADC", &Font20, COL_MENU_HDR, COL_TITLE);
    Paint_DrawString_EN(DISP_W - 52, 12, "ESC", &Font12, COL_MENU_HDR, COL_LABEL);

    /* Dial indicator */
    static constexpr int CX = DISP_W / 2;
    static constexpr int CY = 155;
    static constexpr int R  = 90;

    Paint_DrawCircle(CX, CY, R, COL_TITLE, DOT_PIXEL_1X1, DRAW_FILL_EMPTY);
    Paint_DrawCircle(CX, CY, 3, COL_TITLE, DOT_PIXEL_1X1, DRAW_FILL_FULL);

    bool valid = adc[0].has_value() && adc[1].has_value();
    if (valid) {
        int x_end = CX + static_cast<int>(R * sin(angle));
        int y_end = CY - static_cast<int>(R * cos(angle));
        Paint_DrawLine(CX, CY, x_end, y_end, COL_TITLE, DOT_PIXEL_2X2, LINE_STYLE_SOLID);

        double angle_deg = fmod((angle * 180.0 / M_PI) + 360.0, 360.0);
        char deg_str[24];
        snprintf(deg_str, sizeof(deg_str), "Angle: %.1f", angle_deg);
        Paint_DrawString_EN(12, 258, deg_str, &Font16, COL_BG, COL_BPM);
    } else {
        Paint_DrawString_EN(CX - 20, CY - 8, "----", &Font16, COL_BG, COL_LABEL);
    }

    /* Raw channel values */
    Paint_DrawLine(12, 298, DISP_W - 12, 298, COL_LABEL, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    for (int ch = 0; ch < 4; ++ch) {
        int  x = 12 + (ch % 2) * 112;
        int  y = 304 + (ch / 2) * 14;
        char s[16];
        if (adc[ch].has_value())
            snprintf(s, sizeof(s), "CH%d:%4u", ch, adc[ch].value());
        else
            snprintf(s, sizeof(s), "CH%d:----", ch);
        Paint_DrawString_EN(x, y, s, &Font12, COL_BG, COL_LABEL);
    }

    LCD_2IN_Display(reinterpret_cast<UBYTE *>(buf_));
}

/* ─────────────────────────────────────────────────────────────────────────
 * ProtoSyncUI::drawMidi
 *
 * Layout (240 × 320):
 *   0.. 36   header bar "MIDI"
 *  40.. 56   TX row  — clock indicator + 0xF8 count
 *  60.. 76   RX row  — message counter
 *  78         divider
 *  82.. 94   "STATUS" label
 *  96..120   status hex (Font20)
 * 122..134   "DATA1" / "DATA2" labels
 * 136..156   data hex values (Font16)
 * 158         divider
 * 162..174   "CH" / "CC#" / "VAL" labels
 * 176..196   channel / cc / cc_val values (Font16)
 * 200         divider
 * 206..232   CC value bar (0-127 fill bar)
 * ───────────────────────────────────────────────────────────────────────── */
void ProtoSyncUI::drawMidi(const MidiState &m)
{
    Paint_SelectImage(buf_);
    Paint_Clear(COL_BG);

    char s[32];

    /* ── Header ─────────────────────────────────────────────────────────── */
    Paint_DrawRectangle(0, 0, DISP_W, 36, COL_MENU_HDR, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(12, 9,         "MIDI", &Font20, COL_MENU_HDR, COL_TITLE);
    Paint_DrawString_EN(DISP_W - 52, 12, "ESC", &Font12, COL_MENU_HDR, COL_LABEL);

    /* ── TX row ─────────────────────────────────────────────────────────── */
    UWORD clk_dot = m.clk_running ? COL_BPM : COL_BEAT_OFF;
    Paint_DrawCircle(22, 48, 6, clk_dot, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(34, 42, "TX CLK", &Font12, COL_BG, COL_LABEL);

    snprintf(s, sizeof(s), m.clk_running ? "RUN" : "STOP");
    UWORD clk_col = m.clk_running ? COL_BPM : COL_WARN;
    Paint_DrawString_EN(90, 42, s, &Font12, COL_BG, clk_col);

    snprintf(s, sizeof(s), "%7u", m.clk_count);
    Paint_DrawString_EN(130, 42, s, &Font12, COL_BG, COL_LABEL);

    /* ── RX row ─────────────────────────────────────────────────────────── */
    bool has_rx = (m.rx_count > 0);
    UWORD rx_dot = has_rx ? COL_PEERS : COL_BEAT_OFF;
    Paint_DrawCircle(22, 64, 6, rx_dot, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(34, 58, "RX MSGS", &Font12, COL_BG, COL_LABEL);
    snprintf(s, sizeof(s), "%u", m.rx_count);
    Paint_DrawString_EN(110, 58, s, &Font12, COL_BG, has_rx ? COL_PEERS : COL_LABEL);

    /* ── Divider ────────────────────────────────────────────────────────── */
    Paint_DrawLine(12, 76, DISP_W - 12, 76, COL_LABEL, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawString_EN(12, 80, "LAST RX MESSAGE", &Font12, COL_BG, COL_LABEL);

    /* ── STATUS ─────────────────────────────────────────────────────────── */
    snprintf(s, sizeof(s), "0x%02X", m.status);
    Paint_DrawString_EN(12, 96, s, &Font20, COL_BG, COL_BPM);

    const char *type_str = "----";
    if (m.status != 0) {
        switch (m.status & 0xF0u) {
            case 0x80: type_str = "NOTE OFF";  break;
            case 0x90: type_str = m.data2 ? "NOTE ON" : "NOTE OFF"; break;
            case 0xA0: type_str = "POLY AT";   break;
            case 0xB0: type_str = "CC";        break;
            case 0xC0: type_str = "PROG";      break;
            case 0xD0: type_str = "CHAN AT";   break;
            case 0xE0: type_str = "PBEND";     break;
            default:   type_str = "SYS";       break;
        }
    }
    Paint_DrawString_EN(90, 102, type_str, &Font12, COL_BG, COL_LABEL);

    /* ── DATA1 / DATA2 ──────────────────────────────────────────────────── */
    Paint_DrawLine(12, 122, DISP_W - 12, 122, COL_LABEL, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawString_EN(12,  126, "DATA1", &Font12, COL_BG, COL_LABEL);
    Paint_DrawString_EN(124, 126, "DATA2", &Font12, COL_BG, COL_LABEL);

    snprintf(s, sizeof(s), "0x%02X (%3u)", m.data1, m.data1);
    Paint_DrawString_EN(12,  140, s, &Font12, COL_BG, COL_BPM);
    snprintf(s, sizeof(s), "0x%02X (%3u)", m.data2, m.data2);
    Paint_DrawString_EN(124, 140, s, &Font12, COL_BG, COL_BPM);

    /* ── CHANNEL ────────────────────────────────────────────────────────── */
    Paint_DrawLine(12, 158, DISP_W - 12, 158, COL_LABEL, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawString_EN(12, 162, "CH", &Font12, COL_BG, COL_LABEL);
    snprintf(s, sizeof(s), "%2u", m.channel);
    Paint_DrawString_EN(12, 176, s, &Font20, COL_BG, COL_PEERS);

    /* ── CC# / VAL ──────────────────────────────────────────────────────── */
    Paint_DrawString_EN(80,  162, "CC#", &Font12, COL_BG, COL_LABEL);
    Paint_DrawString_EN(160, 162, "VAL", &Font12, COL_BG, COL_LABEL);
    snprintf(s, sizeof(s), "%3u", m.cc);
    Paint_DrawString_EN(80,  176, s, &Font20, COL_BG, COL_PEERS);
    snprintf(s, sizeof(s), "%3u", m.cc_val);
    Paint_DrawString_EN(160, 176, s, &Font20, COL_BG, COL_PEERS);

    /* ── CC value bar ───────────────────────────────────────────────────── */
    Paint_DrawLine(12, 202, DISP_W - 12, 202, COL_LABEL, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
    Paint_DrawString_EN(12, 206, "CC VAL", &Font12, COL_BG, COL_LABEL);
    static constexpr int CC_BAR_X = 12;
    static constexpr int CC_BAR_Y = 220;
    static constexpr int CC_BAR_W = DISP_W - 24;
    static constexpr int CC_BAR_H = 10;
    Paint_DrawRectangle(CC_BAR_X, CC_BAR_Y,
                        CC_BAR_X + CC_BAR_W, CC_BAR_Y + CC_BAR_H,
                        COL_BEAT_OFF, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    if (m.cc_val > 0) {
        int fill = (static_cast<int>(m.cc_val) * CC_BAR_W) / 127;
        Paint_DrawRectangle(CC_BAR_X, CC_BAR_Y,
                            CC_BAR_X + fill, CC_BAR_Y + CC_BAR_H,
                            COL_PEERS, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    }

    LCD_2IN_Display(reinterpret_cast<UBYTE *>(buf_));
}

/* ─────────────────────────────────────────────────────────────────────────
 * ProtoSyncUI::drawSectionHeader  (private helper)
 * Draws a thin label + hairline separator — matches the style used in other
 * pages so every section looks consistent.
 * ───────────────────────────────────────────────────────────────────────── */
void ProtoSyncUI::drawSectionHeader(int y, const char *label)
{
    Paint_DrawString_EN(12, y, label, &Font12, COL_BG, COL_LABEL);
    Paint_DrawLine(12, y + 14, DISP_W - 12, y + 14,
                   COL_LABEL, DOT_PIXEL_1X1, LINE_STYLE_SOLID);
}

/* ─────────────────────────────────────────────────────────────────────────
 * ProtoSyncUI::drawSparkline  (private helper)
 * Renders the ring-buffer of recent jitter samples as a centred bar chart.
 *
 *   x, y  — top-left corner of the bounding box
 *   w, h  — width and height of the bounding box
 *
 * Bars above the centre baseline = positive jitter (tick arrived late).
 * Bars below                     = negative jitter (tick arrived early).
 *
 * Colour thresholds (match JITTER section traffic-light colours):
 *   green  |jitter| < 500 µs   — within spec
 *   amber  |jitter| < 2000 µs  — marginal
 *   red    |jitter| ≥ 2000 µs  — out of spec
 * ───────────────────────────────────────────────────────────────────────── */
void ProtoSyncUI::drawSparkline(int x, int y, int w, int h,
                                 const MeasureState &ms)
{
    /* Background and centre baseline */
    Paint_DrawRectangle(x, y, x + w, y + h,
                        COL_BEAT_OFF, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    int mid_y = y + h / 2;
    Paint_DrawLine(x, mid_y, x + w, mid_y,
                   COL_LABEL, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    /* Find abs-max for auto-scaling (floor at 500 µs to avoid div/0) */
    float abs_max = 500.0f;
    for (float v : ms.spark)
        if (fabsf(v) > abs_max) abs_max = fabsf(v);

    constexpr int N   = MeasureState::SPARK_N;
    int           bar_w = w / N;
    if (bar_w < 1) bar_w = 1;

    for (int i = 0; i < N; ++i) {
        /* Draw oldest sample at left, newest at right */
        int   idx = (ms.spark_head + i) % N;
        float val = ms.spark[idx];

        int bar_h = static_cast<int>((fabsf(val) / abs_max) * (h / 2));
        if (bar_h < 1) bar_h = 1;

        UWORD col;
        float av = fabsf(val);
        if      (av < 500.0f)  col = COL_GOOD;
        else if (av < 2000.0f) col = COL_WARN;
        else                   col = COL_ERR;

        int bx = x + i * bar_w;
        if (val >= 0.0f) {
            /* Late tick — bar grows upward from baseline */
            Paint_DrawRectangle(bx, mid_y - bar_h, bx + bar_w - 1, mid_y,
                                col, DOT_PIXEL_1X1, DRAW_FILL_FULL);
        } else {
            /* Early tick — bar grows downward from baseline */
            Paint_DrawRectangle(bx, mid_y, bx + bar_w - 1, mid_y + bar_h,
                                col, DOT_PIXEL_1X1, DRAW_FILL_FULL);
        }
    }
}

/* ─────────────────────────────────────────────────────────────────────────
 * ProtoSyncUI::drawMeasurePage  (private shared implementation)
 *
 * Renders a timing-diagnostics page for any MeasureState.
 * Used by both drawMeasure (MIDI 24 PPQN) and drawGpioMeasure (GPIO 4 PPQN).
 *
 *  title        — header bar text, e.g. "MEASURE" or "GPIO CLK"
 *  ms           — pre-copied snapshot (no lock needed)
 *  show_sync_err — true  → draw LINK SYNC ERR section (MIDI page)
 *                  false → draw IDEAL INTERVAL instead (GPIO page)
 *
 * Layout (240 × 320):
 *   0.. 35   header
 *  36.. 89   INTERVAL  — mean + min/max
 *  90..143   JITTER    — 1σ + peak
 * 144..197   SYNC ERR (MIDI) or IDEAL INTERVAL (GPIO)
 * 198..267   SPARKLINE
 * 268..319   footer
 * ───────────────────────────────────────────────────────────────────────── */
void ProtoSyncUI::drawMeasurePage(const char       *title,
                                   const MeasureState &ms,
                                   bool              show_sync_err)
{
    Paint_SelectImage(buf_);
    Paint_Clear(COL_BG);

    char s[32];

    /* ── Header ─────────────────────────────────────────────────────────── */
    Paint_DrawRectangle(0, 0, DISP_W, 36, COL_MENU_HDR, DOT_PIXEL_1X1, DRAW_FILL_FULL);
    Paint_DrawString_EN(12, 9,           title, &Font20, COL_MENU_HDR, COL_TITLE);
    Paint_DrawString_EN(DISP_W - 52, 12, "ESC", &Font12, COL_MENU_HDR, COL_LABEL);

    /* ── INTERVAL ───────────────────────────────────────────────────────── */
    drawSectionHeader(40, "INTERVAL");

    if (ms.sample_count > 0)
        snprintf(s, sizeof(s), "%7.1f", ms.interval_mean_us);
    else
        snprintf(s, sizeof(s), "   ---");
    Paint_DrawString_EN(12, 58, s, &Font20, COL_BG, COL_BPM);
    Paint_DrawString_EN(12 + 6 * 20, 64, "us", &Font12, COL_BG, COL_LABEL);

    snprintf(s, sizeof(s), "mn%7.0f", ms.interval_min_us);
    Paint_DrawString_EN(124, 58, s, &Font12, COL_BG, COL_LABEL);
    snprintf(s, sizeof(s), "mx%7.0f", ms.interval_max_us);
    Paint_DrawString_EN(124, 74, s, &Font12, COL_BG, COL_LABEL);

    /* ── JITTER ─────────────────────────────────────────────────────────── */
    drawSectionHeader(96, "JITTER (period)");

    Paint_DrawString_EN(12, 114, "1\xF4", &Font12, COL_BG, COL_LABEL);
    if (ms.sample_count > 1)
        snprintf(s, sizeof(s), "%6.1f us", ms.interval_stddev_us);
    else
        snprintf(s, sizeof(s), "---");
    UWORD jcol = (ms.interval_stddev_us < 500.0f)  ? COL_GOOD :
                 (ms.interval_stddev_us < 2000.0f) ? COL_WARN : COL_ERR;
    Paint_DrawString_EN(12, 128, s, &Font16, COL_BG, jcol);

    Paint_DrawString_EN(124, 114, "PEAK", &Font12, COL_BG, COL_LABEL);
    snprintf(s, sizeof(s), "%6.1f us", ms.jitter_peak_us);
    UWORD pcol = (ms.jitter_peak_us < 500.0f)  ? COL_GOOD :
                 (ms.jitter_peak_us < 2000.0f) ? COL_WARN : COL_ERR;
    Paint_DrawString_EN(124, 128, s, &Font16, COL_BG, pcol);

    /* ── SYNC ERR (MIDI) or IDEAL INTERVAL (GPIO) ───────────────────────── */
    if (show_sync_err) {
        drawSectionHeader(150, "LINK SYNC ERR");

        snprintf(s, sizeof(s), "%+7.1f us", ms.sync_error_us);
        UWORD ecol = (fabs(ms.sync_error_us) < 500.0)  ? COL_GOOD :
                     (fabs(ms.sync_error_us) < 2000.0) ? COL_WARN : COL_ERR;
        Paint_DrawString_EN(12, 168, s, &Font20, COL_BG, ecol);

        /* Bidirectional bar ±5 ms clamped */
        static constexpr int EB_X = 12;
        static constexpr int EB_Y = 192;
        static constexpr int EB_W = DISP_W - 24;
        static constexpr int EB_H = 6;
        int mid = EB_X + EB_W / 2;

        Paint_DrawRectangle(EB_X, EB_Y, EB_X + EB_W, EB_Y + EB_H,
                            COL_BEAT_OFF, DOT_PIXEL_1X1, DRAW_FILL_FULL);

        double clamped = ms.sync_error_us;
        if (clamped >  5000.0) clamped =  5000.0;
        if (clamped < -5000.0) clamped = -5000.0;
        int fill = static_cast<int>((clamped / 5000.0) * (EB_W / 2));
        if (fill > 0)
            Paint_DrawRectangle(mid, EB_Y, mid + fill, EB_Y + EB_H,
                                ecol, DOT_PIXEL_1X1, DRAW_FILL_FULL);
        else if (fill < 0)
            Paint_DrawRectangle(mid + fill, EB_Y, mid, EB_Y + EB_H,
                                ecol, DOT_PIXEL_1X1, DRAW_FILL_FULL);

        Paint_DrawLine(mid, EB_Y - 2, mid, EB_Y + EB_H + 2,
                       COL_LABEL, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    } else {
        /* GPIO page: show ideal interval at current BPM instead */
        drawSectionHeader(150, "IDEAL INTERVAL");

        Paint_DrawString_EN(12, 168, "60000000/BPM/PPQN", &Font12, COL_BG, COL_LABEL);

        /* Show the ideal vs actual deviation as a simple bar */
        static constexpr int IB_X = 12;
        static constexpr int IB_Y = 188;
        static constexpr int IB_W = DISP_W - 24;
        static constexpr int IB_H = 6;

        /* Background */
        Paint_DrawRectangle(IB_X, IB_Y, IB_X + IB_W, IB_Y + IB_H,
                            COL_BEAT_OFF, DOT_PIXEL_1X1, DRAW_FILL_FULL);

        /* Fill proportional to how close actual mean is to the range
         * [0, 2×mean] centred on itself — always full green when perfect */
        if (ms.sample_count > 0) {
            float ratio = ms.interval_mean_us > 0.0f
                ? (ms.interval_mean_us - ms.interval_stddev_us) / ms.interval_mean_us
                : 0.0f;
            if (ratio < 0.0f) ratio = 0.0f;
            if (ratio > 1.0f) ratio = 1.0f;
            int fill_w = static_cast<int>(ratio * IB_W);
            UWORD fcol = (ms.interval_stddev_us < 500.0f)  ? COL_GOOD :
                         (ms.interval_stddev_us < 2000.0f) ? COL_WARN : COL_ERR;
            Paint_DrawRectangle(IB_X, IB_Y, IB_X + fill_w, IB_Y + IB_H,
                                fcol, DOT_PIXEL_1X1, DRAW_FILL_FULL);
        }

        snprintf(s, sizeof(s), "last: %+7.1f us", ms.jitter_last_us);
        Paint_DrawString_EN(12, 200, s, &Font12, COL_BG,
                            (fabsf(ms.jitter_last_us) < 500.0f)  ? COL_GOOD :
                            (fabsf(ms.jitter_last_us) < 2000.0f) ? COL_WARN : COL_ERR);
    }

    /* ── SPARKLINE ──────────────────────────────────────────────────────── */
    Paint_DrawString_EN(12, 210, "HISTORY", &Font12, COL_BG, COL_LABEL);
    drawSparkline(12, 224, DISP_W - 24, 44, ms);

    /* ── Footer ─────────────────────────────────────────────────────────── */
    Paint_DrawLine(12, 274, DISP_W - 12, 274,
                   COL_LABEL, DOT_PIXEL_1X1, LINE_STYLE_SOLID);

    Paint_DrawString_EN(12, 280, "SAMPLES", &Font12, COL_BG, COL_LABEL);
    snprintf(s, sizeof(s), "%u", ms.sample_count);
    Paint_DrawString_EN(12, 296, s, &Font16, COL_BG, COL_TITLE);
    Paint_DrawString_EN(100, 296, "R=reset", &Font12, COL_BG, COL_LABEL);

    LCD_2IN_Display(reinterpret_cast<UBYTE *>(buf_));
}

/* ─────────────────────────────────────────────────────────────────────────
 * ProtoSyncUI::drawMeasure  — MIDI 24 PPQN diagnostics (with sync error)
 * ───────────────────────────────────────────────────────────────────────── */
void ProtoSyncUI::drawMeasure(const MeasureState &ms)
{
    drawMeasurePage("MEASURE", ms, true);
}

/* ─────────────────────────────────────────────────────────────────────────
 * ProtoSyncUI::drawGpioMeasure  — GPIO 4 PPQN diagnostics
 * ───────────────────────────────────────────────────────────────────────── */
void ProtoSyncUI::drawGpioMeasure(const MeasureState &ms)
{
    drawMeasurePage("GPIO CLK", ms, false);
}