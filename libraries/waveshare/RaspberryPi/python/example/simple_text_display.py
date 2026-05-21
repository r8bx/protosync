#!/usr/bin/python
# -*- coding: UTF-8 -*-
import os
import sys 
import time
import logging
import spidev as SPI
sys.path.append("..")
from lib import LCD_2inch
from PIL import Image,ImageDraw,ImageFont
# Raspberry Pi pin configuration:
RST = 27
DC = 25
BL = 18
bus = 0 
device = 0 
logging.basicConfig(level=logging.DEBUG)
try:
    # display with hardware SPI:
    disp = LCD_2inch.LCD_2inch()
    # Initialize library.
    disp.Init()
    # Clear display.
    disp.clear()
    # Set the backlight to 50
    disp.bl_DutyCycle(50)
    isRunning = True          # ← fixed: 4 spaces
    while isRunning:          # ← fixed: 4 spaces
        # Create blank image for drawing with black background.
        image1 = Image.new("RGB", (disp.width, disp.height), "BLACK")
        draw = ImageDraw.Draw(image1)
        # Load font                        # ← fixed: 8 spaces
        Font1 = ImageFont.truetype("../Font/Roboto-Regular.ttf", 25)
        # Draw two lines of white text on black background
        draw.text((5, 10), 'Hello world!', fill="WHITE", font=Font1)
        draw.text((5, 40), '1This is Line 2', fill="WHITE", font=Font1)
        # Rotate image 180 degrees (as per example)
        image1 = image1.rotate(180)
        disp.ShowImage(image1)

    time.sleep(3)
    disp.module_exit()
    logging.info("Display complete")
except IOError as e:
    logging.info(e) 
except KeyboardInterrupt:
    disp.module_exit()
    logging.info("quit:")
    exit()