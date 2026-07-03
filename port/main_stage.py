# Stage (python-ugame game library) demo on the SNES: a brick-walled arena
# on the BG layer and six hardware sprites bouncing around it. Frozen into
# build/mpystage.sfc; tests assert the mailbox transcript, the screen shows
# the game. Unlike nano-gui (a framebuffer repainted in Python), all the
# drawing here is PPU silicon — Python only moves the sprites.
import stage
import stage_assets

bank = stage.Bank(stage_assets.BANK, stage_assets.PALETTE)
game = stage.Stage(fps=60)  # no pacing: run as fast as the loop can

# 16x14 cells of 16px = the full 256x224 screen: brick border, dotted floor
grid = stage.Grid(bank, 16, 14)
for x in range(16):
    grid.tile(x, 0, 1)
    grid.tile(x, 13, 1)
for y in range(14):
    grid.tile(0, y, 1)
    grid.tile(15, y, 1)
for y in range(1, 13):
    for x in range(1, 15):
        grid.tile(x, y, 2)

balls = []
for (x, y, dx, dy, f) in ((24, 24, 2, 1, 3), (120, 60, -2, 2, 4),
                          (200, 100, 1, -2, 3), (60, 150, -1, -1, 4),
                          (160, 40, 2, 2, 5), (48, 96, -2, -1, 5)):
    b = stage.Sprite(bank, f, x, y)
    b.dx = dx
    b.dy = dy
    balls.append(b)

game.layers = balls + [grid]
game.render_block()
print("stage: init ok")

for frame in range(120):
    for b in balls:
        x = b.x + b.dx
        y = b.y + b.dy
        if x < 16 or x > 224:
            b.dx = -b.dx
            x = b.x + b.dx
        if y < 16 or y > 192:
            b.dy = -b.dy
            y = b.y + b.dy
        b.move(x, y)
    game.render_sprites(balls)
    if frame % 30 == 0:
        print("stage: frame", frame)

print("stage: done")
