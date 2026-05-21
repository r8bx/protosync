#!/usr/bin/python
# -*- coding: UTF-8 -*-
"""
Scrollable menu for LCD_2inch display with keyboard navigation.
Controls:
  UP / DOWN arrow  — move selection
  ENTER            — confirm selection
  ESC or Q         — quit
"""

import sys
import time
import logging
import curses

sys.path.append("..")
from lib import LCD_2inch
from PIL import Image, ImageDraw, ImageFont

# ── Display hardware config ──────────────────────────────────────────────────
RST    = 27
DC     = 25
BL     = 18
bus    = 0
device = 0

# ── Visual config ────────────────────────────────────────────────────────────
FONT_PATH      = "../Font/Roboto-Regular.ttf"
FONT_SIZE      = 26
ITEM_HEIGHT    = 36          # px per menu row
PAD_X          = 12          # left text margin
PAD_TOP        = 8           # top margin before first item
SCROLLBAR_W    = 5           # scrollbar strip width

COLOR_BG       = (0,   0,   0)      # black background
COLOR_TEXT     = (220, 220, 220)    # light grey text
COLOR_SEL_BG   = (30,  100, 210)   # blue highlight bar
COLOR_SEL_TEXT = (255, 255, 255)   # white selected text
COLOR_SCROLL   = (60,  60,  60)    # scrollbar track
COLOR_THUMB    = (160, 160, 160)   # scrollbar thumb
COLOR_HEADER   = (80,  80,  80)    # header bar
COLOR_HDR_TEXT = (200, 200, 200)   # header text

HEADER_H   = 32              # header bar height (0 to disable)
HEADER_TXT = "MAIN MENU"

# ── Menu items — edit freely ─────────────────────────────────────────────────
MENU_ITEMS = [
    "System Info",
    "Network Settings",
    "Display Brightness",
    "Sound Settings",
    "Bluetooth",
    "Wi-Fi",
    "Storage",
    "Date & Time",
    "Shutdown",
    "Restart",
]


# ── Rendering ────────────────────────────────────────────────────────────────

def render_menu(disp, font, small_font, items, selected, scroll_off, visible_n):
    w, h = disp.width, disp.height
    img  = Image.new("RGB", (w, h), COLOR_BG)
    drw  = ImageDraw.Draw(img)

    # Header bar
    if HEADER_H:
        drw.rectangle([(0, 0), (w, HEADER_H - 1)], fill=COLOR_HEADER)
        drw.text((PAD_X, (HEADER_H - FONT_SIZE) // 2), HEADER_TXT,
                 fill=COLOR_HDR_TEXT, font=small_font)
        # subtle separator line
        drw.line([(0, HEADER_H - 1), (w, HEADER_H - 1)], fill=(100, 100, 100))

    content_y0 = HEADER_H if HEADER_H else 0

    # Scrollbar track
    drw.rectangle([(w - SCROLLBAR_W, content_y0), (w, h)], fill=COLOR_SCROLL)

    # Scrollbar thumb
    total = len(items)
    if total > visible_n:
        track_h  = h - content_y0
        thumb_h  = max(20, track_h * visible_n // total)
        thumb_y  = content_y0 + track_h * scroll_off // total
        drw.rectangle(
            [(w - SCROLLBAR_W, thumb_y), (w, thumb_y + thumb_h)],
            fill=COLOR_THUMB
        )

    # Menu rows
    for row in range(visible_n):
        idx = scroll_off + row
        if idx >= total:
            break

        y     = content_y0 + PAD_TOP + row * ITEM_HEIGHT
        y_end = y + ITEM_HEIGHT - 2
        is_sel = idx == selected

        if is_sel:
            drw.rectangle([(0, y - 2), (w - SCROLLBAR_W - 1, y_end)],
                          fill=COLOR_SEL_BG)
            # small arrow indicator
            arrow_x = w - SCROLLBAR_W - 14
            mid_y   = (y + y_end) // 2
            drw.polygon(
                [(arrow_x, mid_y - 5),
                 (arrow_x, mid_y + 5),
                 (arrow_x + 8, mid_y)],
                fill=COLOR_SEL_TEXT
            )
            drw.text((PAD_X, y + 4), items[idx],
                     fill=COLOR_SEL_TEXT, font=font)
        else:
            drw.text((PAD_X, y + 4), items[idx],
                     fill=COLOR_TEXT, font=font)

    # Item counter (bottom-right corner)
    counter = f"{selected + 1}/{total}"
    drw.text((w - SCROLLBAR_W - 40, h - 18), counter,
             fill=(90, 90, 90), font=small_font)

    img = img.rotate(270)
    disp.ShowImage(img)


def show_selection_screen(disp, font, small_font, label):
    w, h = disp.width, disp.height
    img  = Image.new("RGB", (w, h), COLOR_BG)
    drw  = ImageDraw.Draw(img)

    # Accent rectangle
    drw.rectangle([(20, h // 2 - 50), (w - 20, h // 2 + 50)],
                  fill=(20, 70, 150))
    drw.rectangle([(22, h // 2 - 48), (w - 22, h // 2 + 48)],
                  outline=(60, 130, 220), width=1)

    drw.text((PAD_X + 8, h // 2 - 40), "Selected:",
             fill=(180, 180, 180), font=small_font)
    drw.text((PAD_X + 8, h // 2 - 10), label,
             fill=COLOR_SEL_TEXT, font=font)

    img = img.rotate(270)
    disp.ShowImage(img)


# ── Main ─────────────────────────────────────────────────────────────────────

def main(stdscr):
    logging.basicConfig(level=logging.DEBUG)

    # Init display
    disp = LCD_2inch.LCD_2inch()
    disp.Init()
    disp.clear()
    disp.bl_DutyCycle(50)

    font       = ImageFont.truetype(FONT_PATH, FONT_SIZE)
    small_font = ImageFont.truetype(FONT_PATH, 18)

    content_h = disp.height - (HEADER_H if HEADER_H else 0)
    visible_n = (content_h - PAD_TOP) // ITEM_HEIGHT

    selected   = 0
    scroll_off = 0
    redraw     = True

    # curses setup — no echo, instant key response, arrow-key support
    curses.curs_set(0)
    stdscr.nodelay(False)
    stdscr.keypad(True)
    stdscr.addstr(0, 0, "LCD Menu active — arrows to navigate, Enter to select, Q/ESC to quit")
    stdscr.refresh()

    try:
        while True:
            if redraw:
                render_menu(disp, font, small_font,
                            MENU_ITEMS, selected, scroll_off, visible_n)
                # Mirror state in terminal too
                stdscr.move(2, 0)
                stdscr.clrtoeol()
                stdscr.addstr(2, 0, f"  > {MENU_ITEMS[selected]}  ({selected+1}/{len(MENU_ITEMS)})")
                stdscr.refresh()
                redraw = False

            key = stdscr.getch()

            if key == curses.KEY_UP:
                if selected > 0:
                    selected -= 1
                    if selected < scroll_off:
                        scroll_off -= 1
                    redraw = True

            elif key == curses.KEY_DOWN:
                if selected < len(MENU_ITEMS) - 1:
                    selected += 1
                    if selected >= scroll_off + visible_n:
                        scroll_off += 1
                    redraw = True

            elif key in (curses.KEY_ENTER, 10, 13):
                label = MENU_ITEMS[selected]
                show_selection_screen(disp, font, small_font, label)
                stdscr.move(3, 0)
                stdscr.clrtoeol()
                stdscr.addstr(3, 0, f"  Activated: {label}")
                stdscr.refresh()
                time.sleep(1.5)
                redraw = True

            elif key in (ord('q'), ord('Q'), 27):   # Q or ESC
                break

    finally:
        disp.module_exit()
        logging.info("Display exited cleanly.")


if __name__ == "__main__":
    curses.wrapper(main)
    sys.exit(0)
