# The Stage game library, SNES edition.
#
# Ported from python-ugame/micropython-stage (MIT License,
# Copyright (c) 2018 Radomir Dopieralski) — see STAGE_LICENSE.
# The Python API (Bank, Grid, WallGrid, Sprite, Stage, collide, color565)
# is kept compatible; the differences from upstream:
#   - rendering is done by the SNES PPU via the _snesstage C module, not by
#     the per-pixel _stage compositor: banks upload to VRAM once, a Grid is
#     the BG1 tilemap (Grid.move = scroll registers), a Sprite is an OAM
#     entry. Changes become visible at render_block()/render_sprites().
#   - no filesystem on a SNES cartridge: the BMP16/GIF16 loaders are gone,
#     Banks are built from bytes (frozen assets).
#   - one Grid layer is hardware-backed (the first one in Stage.layers);
#     custom per-layer palettes fall back to the bank's palette.
#   - Sprite rotations 0/2/4/6 map to OAM H/V flips; the 90-degree ones
#     (odd values) would need pre-rotated tiles and raise ValueError.
#   - Text is not implemented yet (it wants a 2bpp BG3 backend).

import _snesstage


def color565(r, g, b):
    return (r & 0xf8) << 8 | (g & 0xfc) << 3 | b >> 3


def collide(ax0, ay0, ax1, ay1, bx0, by0, bx1=None, by1=None):
    if bx1 is None:
        bx1 = bx0
    if by1 is None:
        by1 = by0
    return not (ax1 < bx0 or ay1 < by0 or ax0 > bx1 or ay0 > by1)


class Bank:
    """16 tiles of 16x16 pixels, 4 bits per pixel, plus a 16-color
    RGB565 (big-endian) palette. Uploaded to a VRAM slot on first use."""

    _next_slot = 0

    def __init__(self, buffer=None, palette=None):
        self.buffer = buffer
        self.palette = palette
        self.slot = None

    def _upload(self):
        if self.slot is None:
            slot = Bank._next_slot
            if slot >= 8:
                raise ValueError("out of VRAM bank slots (8)")
            _snesstage.bank(slot, self.buffer, self.palette)
            Bank._next_slot = slot + 1
            self.slot = slot
        return self.slot


class Grid:
    def __init__(self, bank, width=8, height=8, palette=None, buffer=None):
        self.x = 0
        self.y = 0
        self.z = 0
        self.stride = (width + 1) & 0xfe
        self.width = width
        self.height = height
        self.bank = bank
        self.buffer = buffer or bytearray(self.stride * height)

    def tile(self, x, y, tile=None):
        if not 0 <= x < self.width or not 0 <= y < self.height:
            return 0
        index = (y * self.stride + x) >> 1
        b = self.buffer[index]
        if tile is None:
            return b & 0x0f if x & 0x01 else b >> 4
        if x & 0x01:
            b = b & 0xf0 | tile
        else:
            b = b & 0x0f | (tile << 4)
        self.buffer[index] = b

    def move(self, x, y, z=None):
        self.x = x
        self.y = y
        if z is not None:
            self.z = z


class WallGrid(Grid):
    def __init__(self, grid, walls, bank, palette=None):
        super().__init__(bank, grid.width + 1, grid.height + 1, palette)
        self.grid = grid
        self.walls = walls
        self.update()
        self.move(self.x - 8, self.y - 8)

    def update(self):
        for y in range(self.height):
            for x in range(self.width):
                t = 0
                bit = 1
                for dy in (-1, 0):
                    for dx in (-1, 0):
                        if self.grid.tile(x + dx, y + dy) in self.walls:
                            t |= bit
                        bit <<= 1
                self.tile(x, y, t)


class Sprite:
    _next_oam = 0

    def __init__(self, bank, frame, x, y, z=0, rotation=0, palette=None):
        self.bank = bank
        self.frame = frame
        self.rotation = rotation
        self.x = x
        self.y = y
        self.z = z
        self.oam = Sprite._next_oam
        Sprite._next_oam += 1
        self.px = x
        self.py = y
        self._slot = bank._upload()
        self._sync()

    def move(self, x, y, z=None):
        # the per-frame hot path: one C call updates the OAM shadow
        self.x = x
        self.y = y
        if z is not None:
            self.z = z
        _snesstage.sprite(self.oam, x, y, self.frame, self.rotation,
                          self._slot, self._slot)

    def set_frame(self, frame=None, rotation=None):
        if frame is not None:
            self.frame = frame
        if rotation is not None:
            self.rotation = rotation
        self._sync()

    def update(self):
        pass

    def _updated(self):
        self.px = int(self.x)
        self.py = int(self.y)

    def _sync(self):
        if self.rotation & 1:
            raise ValueError("90-degree rotation not supported on SNES OBJs")
        _snesstage.sprite(self.oam, int(self.x), int(self.y),
                          self.frame, self.rotation, self._slot, self._slot)


class Stage:
    def __init__(self, display=None, fps=12, scale=None):
        self.layers = []
        self.display = display
        self.scale = 1
        self.width = _snesstage.WIDTH
        self.height = _snesstage.HEIGHT
        # No eager _snesstage.init() here: the C module lazy-inits on first
        # hardware touch, which keeps the boot console (and prints) on
        # screen through the seconds of Python-side game setup (the M7
        # lesson: don't blank the screen a minute before the first frame).
        self.tick_frames = max(1, 60 // fps)

    def tick(self):
        _snesstage.vsync(self.tick_frames)

    def _sync_grid(self, full):
        for l in self.layers:
            if isinstance(l, Grid):
                if full:
                    _snesstage.grid(l.buffer, l.stride, l.width, l.height,
                                    l.bank._upload(), l.bank._upload())
                _snesstage.scroll(int(l.x), int(l.y))
                return

    def render_block(self, x0=0, y0=0, x1=None, y1=None):
        self._sync_grid(True)
        _snesstage.flip()

    def render_sprites(self, sprites):
        self._sync_grid(False)
        for s in sprites:
            s._updated()
        _snesstage.flip()
