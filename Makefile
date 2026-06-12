CALDIR := toolchain/usr/local/lib/calypsi-65816-5.17
CC := $(CALDIR)/bin/cc65816
AS := $(CALDIR)/bin/as65816
LN := $(CALDIR)/bin/ln65816

MODEL := --code-model=large --data-model=large
CFLAGS = --core=65816 --target=snes $(MODEL) --no-ppu-mul -O2 --list-file=$(@:.o=.lst)
AFLAGS := --core=65816 --target=SNES $(MODEL)
LNFLAGS := snes/linker.scm --output-format=raw --rtattr exit=simplified \
           --cross-reference $(CALDIR)/lib/clib-lc-ld-snes-noppu.a

BUILD := build

SNES_OBJS := $(BUILD)/header.o $(BUILD)/mailbox.o

.PHONY: all clean hello selftest
all: hello selftest
hello: $(BUILD)/hello.sfc
selftest: $(BUILD)/selftest.sfc

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/header.o: snes/header.s | $(BUILD)
	$(AS) -o $@ $< $(AFLAGS)

$(BUILD)/%.o: snes/%.c snes/mailbox.h | $(BUILD)
	$(CC) -o $@ $< $(CFLAGS)

$(BUILD)/hello_main.o: m0_hello/main.c snes/mailbox.h | $(BUILD)
	$(CC) -o $@ $< $(CFLAGS)

$(BUILD)/selftest_main.o: m1_selftest/main.c snes/mailbox.h | $(BUILD)
	$(CC) -o $@ $< $(CFLAGS)

$(BUILD)/hello.raw: $(SNES_OBJS) $(BUILD)/hello_main.o
	$(LN) -o $@ $(LNFLAGS) --list-file=$(BUILD)/hello.map $^

$(BUILD)/selftest.raw: $(SNES_OBJS) $(BUILD)/selftest_main.o
	$(LN) -o $@ $(LNFLAGS) --list-file=$(BUILD)/selftest.map $^

$(BUILD)/%.sfc: $(BUILD)/%.raw tools/raw2sfc.py
	python3 tools/raw2sfc.py $< $@

clean:
	rm -rf $(BUILD)
