// On-screen keyboard for the SNES REPL: a 13x4 character grid on the bottom
// rows of the text console, driven by joypad 1. Name-entry style: D-pad
// moves (with autorepeat), A types, B backspace, X space, Y toggles the
// caps/symbols page, Start = Enter, Select = Ctrl-D (exit).
#ifndef SNES_OSKB_H
#define SNES_OSKB_H

void oskb_init(void); // draw separator, grid and button hints
int oskb_poll(void); // one joypad sample; returns a char (\b \n 0x04 ...) or -1

#endif
