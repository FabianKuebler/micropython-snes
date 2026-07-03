# nano-gui demo on the SNES: meter, LEDs, labels and an analog dial, driven
# through peterhinch/micropython-nano-gui running unmodified on the frozen
# package tree. Frozen into build/mpygui.sfc; tests assert the mailbox
# transcript, the screen shows the widgets.
from color_setup import ssd
from gui.core.nanogui import refresh, DObject
from gui.widgets.label import Label
from gui.widgets.meter import Meter
from gui.widgets.led import LED
from gui.widgets.dial import Dial, Pointer

# Register the device by hand (= refresh()'s first-visit branch minus the
# show): the first ssd.show() hands the PPU from the boot console to the
# framebuffer, and widget construction below takes ~1 min of real time on
# the 65816 — keep the console (and these prints) on screen until the
# first complete frame.
DObject.devices[ssd] = set()
ssd.fill(0)
print("nanogui: init ok")

from gui.core.writer import CWriter
import gui.fonts.arial10 as arial10
from gui.core.colors import *

CWriter.set_textpos(ssd, 0, 0)
wri = CWriter(ssd, arial10, GREEN, BLACK, verbose=False)

Label(wri, 2, 2, "MicroPython nano-gui on SNES")

meter = Meter(wri, 20, 8, height=100, width=16, divisions=4,
              legends=("0", "50", "100"), label="pwr")
led = LED(wri, 20, 60)  # kwargs form hits an open Calypsi roulette site
led.color(YELLOW)
dial = Dial(wri, 20, 120, height=100, ticks=12, fgcolor=CYAN,
            label="clock")
ptr = Pointer(dial)

val_lbl = Label(wri, 140, 8, 60, bgcolor=DARKGREEN)

steps = (0.1, 0.45, 0.8)
for i, v in enumerate(steps):
    meter.value(v)
    led.color(RED if v > 0.7 else YELLOW)
    ptr.value(0.4 + 0.4j * (i + 1) / 3, RED)
    val_lbl.value("v={}".format(v))
    refresh(ssd)
    print("nanogui: frame", i, "value", "%.2f" % v)

print("nanogui: done")
