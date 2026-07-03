# color_setup for micropython-nano-gui on the SNES.
#
# The SSD driver is a framebuf.FrameBuffer over a 256x192 GS4_HMSB bytearray
# (16 colors). nano-gui's 4-bit LUT driver model maps 1:1 onto SNES hardware:
# CWriter.create_color() fills SSD.lut with 16-bit colors and draws with the
# 4-bit *index*; show() programs the CGRAM palette from the lut and hands the
# buffer to the snesfb C module (nibbles -> bitplanes -> VRAM via DMA).
import framebuf
import snesfb
from drivers.boolpalette import BoolPalette


class SSD(framebuf.FrameBuffer):
    lut = bytearray(32)

    # rgb() returns a SNES BGR555 color word; nano-gui stores it in the lut.
    @staticmethod
    def rgb(r, g, b):
        return ((b & 0xF8) << 7) | ((g & 0xF8) << 2) | (r >> 3)

    def __init__(self):
        self.width = snesfb.WIDTH
        self.height = snesfb.HEIGHT
        self.buffer = bytearray(self.width * self.height // 2)
        self.mode = framebuf.GS4_HMSB
        self.palette = BoolPalette(self.mode)
        super().__init__(self.buffer, self.width, self.height, self.mode)
        self._hw = False

    def show(self):
        # PPU takeover deferred to the first real frame: imports take a
        # minute of real time on the 65816 and the boot console shows
        # progress until then (snesfb.init() disables it).
        if not self._hw:
            snesfb.init()
            self._hw = True
        lut = SSD.lut
        for i in range(16):
            snesfb.palette(i, lut[2 * i] | (lut[2 * i + 1] << 8))
        snesfb.show(self.buffer)


ssd = SSD()
