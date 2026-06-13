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

# ---- M2+: MicroPython core built with Calypsi -------------------------------

MPTOP := micropython
PORT := port
MPBUILD := $(BUILD)/mpy
GENHDR := $(MPBUILD)/genhdr
PYTHON := python3
# Host gcc only preprocesses for the qstr/module/root-pointer extraction
# scripts; type sizes don't matter there, visibility of config-gated code does.
QCPP := gcc -E
MP_INC := -I$(PORT) -I$(MPTOP) -I$(MPBUILD)
MPCFLAGS = --core=65816 --target=snes $(MODEL) --no-ppu-mul -O1 --no-cross-call -DNDEBUG \
           $(MP_INC) --list-file=$(@:.o=.lst)

PY_SRC_NAMES := \
	mpstate.c nlr.c nlrx86.c nlrx64.c nlrthumb.c nlraarch64.c nlrmips.c \
	nlrpowerpc.c nlrxtensa.c nlrrv32.c nlrrv64.c nlrsetjmp.c malloc.c gc.c \
	pystack.c qstr.c vstr.c mpprint.c unicode.c mpz.c reader.c lexer.c \
	parse.c scope.c compile.c emitcommon.c emitbc.c asmbase.c asmx64.c \
	emitnx64.c asmx86.c emitnx86.c asmthumb.c emitnthumb.c emitinlinethumb.c \
	asmarm.c emitnarm.c asmxtensa.c emitnxtensa.c emitinlinextensa.c \
	emitnxtensawin.c asmrv32.c emitnrv32.c emitinlinerv32.c emitndebug.c \
	formatfloat.c parsenumbase.c parsenum.c emitglue.c persistentcode.c \
	runtime.c runtime_utils.c scheduler.c nativeglue.c pairheap.c ringbuf.c \
	cstack.c stackctrl.c argcheck.c warning.c profile.c map.c obj.c \
	objarray.c objattrtuple.c objbool.c objboundmeth.c objcell.c \
	objclosure.c objcode.c objcomplex.c objdeque.c objdict.c objenumerate.c \
	objexcept.c objfilter.c objfloat.c objfun.c objgenerator.c \
	objgetitemiter.c objint.c objint_longlong.c objint_mpz.c objlist.c \
	objmap.c objmodule.c objobject.c objpolyiter.c objproperty.c objnone.c \
	objnamedtuple.c objrange.c objreversed.c objringio.c objset.c \
	objsingleton.c objslice.c objstr.c objstrunicode.c objstringio.c \
	objtemplate.c objtuple.c objtype.c objzip.c opmethods.c sequence.c \
	stream.c binary.c builtinimport.c builtinevex.c builtinhelp.c \
	modarray.c modbuiltins.c modcollections.c modgc.c modio.c modmath.c \
	modcmath.c modmicropython.c modstring.c modstruct.c modsys.c \
	moderrno.c modthread.c modweakref.c vm.c bc.c showbc.c repl.c \
	smallint.c frozenmod.c

PY_OBJS := $(addprefix $(MPBUILD)/py/,$(PY_SRC_NAMES:.c=.o))
PORT_OBJS := $(MPBUILD)/main.o $(SNES_OBJS)
SRC_QSTR := $(addprefix $(MPTOP)/py/,$(filter-out nlr%,$(PY_SRC_NAMES))) \
            $(PORT)/main.c

.PHONY: mpy patch-micropython
mpy: $(BUILD)/mpy.sfc

# Tiny upstream patches (see patches/ and DECISIONS.md), applied idempotently.
patch-micropython:
	@cd $(MPTOP) && for p in ../patches/*.patch; do \
	  git apply --reverse --check $$p 2>/dev/null && continue; \
	  git apply $$p && echo "applied $$p"; \
	done

$(GENHDR):
	mkdir -p $(GENHDR) $(MPBUILD)/py

$(GENHDR)/mpversion.h: | $(GENHDR)
	$(PYTHON) $(MPTOP)/py/makeversionhdr.py $@

$(GENHDR)/qstr.i.last: $(SRC_QSTR) $(PORT)/mpconfigport.h | $(GENHDR)/mpversion.h
	$(PYTHON) $(MPTOP)/py/makeqstrdefs.py pp $(QCPP) output $@ \
	  cflags $(MP_INC) -DNO_QSTR -DMICROPY_PREVIEW_VERSION_2=0 \
	  cxxflags sources $(SRC_QSTR) dependencies $(PORT)/mpconfigport.h \
	  changed_sources $?

$(GENHDR)/qstrdefs.collected.h: $(GENHDR)/qstr.i.last
	$(PYTHON) $(MPTOP)/py/makeqstrdefs.py split qstr $< $(GENHDR)/qstr _
	$(PYTHON) $(MPTOP)/py/makeqstrdefs.py cat qstr _ $(GENHDR)/qstr $@

$(GENHDR)/moduledefs.collected: $(GENHDR)/qstr.i.last
	$(PYTHON) $(MPTOP)/py/makeqstrdefs.py split module $< $(GENHDR)/module _
	$(PYTHON) $(MPTOP)/py/makeqstrdefs.py cat module _ $(GENHDR)/module $@

$(GENHDR)/root_pointers.collected: $(GENHDR)/qstr.i.last
	$(PYTHON) $(MPTOP)/py/makeqstrdefs.py split root_pointer $< $(GENHDR)/root_pointer _
	$(PYTHON) $(MPTOP)/py/makeqstrdefs.py cat root_pointer _ $(GENHDR)/root_pointer $@

$(GENHDR)/qstrdefs.generated.h: $(GENHDR)/qstrdefs.collected.h $(PORT)/qstrdefsport.h
	cat $(MPTOP)/py/qstrdefs.h $(PORT)/qstrdefsport.h $(GENHDR)/qstrdefs.collected.h \
	  | sed 's/^Q(.*)/"&"/' \
	  | $(QCPP) $(MP_INC) -DNO_QSTR -DMICROPY_PREVIEW_VERSION_2=0 - \
	  | sed 's/^\"\(Q(.*)\)\"/\1/' > $(GENHDR)/qstrdefs.preprocessed.h
	$(PYTHON) $(MPTOP)/py/makeqstrdata.py $(GENHDR)/qstrdefs.preprocessed.h > $@

$(GENHDR)/moduledefs.h: $(GENHDR)/moduledefs.collected
	$(PYTHON) $(MPTOP)/py/makemoduledefs.py $< > $@

$(GENHDR)/root_pointers.h: $(GENHDR)/root_pointers.collected
	$(PYTHON) $(MPTOP)/py/make_root_pointers.py $< > $@

GENERATED := $(GENHDR)/mpversion.h $(GENHDR)/qstrdefs.generated.h \
             $(GENHDR)/moduledefs.h $(GENHDR)/root_pointers.h

# vm.c's 256-case dispatch switch lands in the weeds with the default
# strategy; if-else compare chains work (Calypsi bug, DECISIONS.md)
$(MPBUILD)/py/vm.o: MPCFLAGS += --force-switch if-else

$(MPBUILD)/py/%.o: $(MPTOP)/py/%.c $(PORT)/mpconfigport.h | $(GENERATED)
	$(CC) -o $@ $< $(MPCFLAGS)

$(MPBUILD)/main.o: $(PORT)/main.c $(PORT)/mpconfigport.h snes/mailbox.h | $(GENERATED)
	$(CC) -o $@ $< $(MPCFLAGS)

# ---- frozen bytecode: host mpy-cross + mpy-tool ------------------------------

MPY_CROSS := $(MPTOP)/mpy-cross/build/mpy-cross

$(MPY_CROSS):
	$(MAKE) -C $(MPTOP)/mpy-cross

$(MPBUILD)/main.mpy: $(PORT)/main.py $(MPY_CROSS) | $(GENHDR)
	$(MPY_CROSS) -o $@ -s main.py $<

$(MPBUILD)/frozen_content.c: $(MPBUILD)/main.mpy $(GENHDR)/qstrdefs.generated.h
	$(PYTHON) $(MPTOP)/tools/mpy-tool.py -f -q $(GENHDR)/qstrdefs.preprocessed.h \
	  -mlongint-impl=none $< > $@

$(MPBUILD)/frozen_content.o: $(MPBUILD)/frozen_content.c
	$(CC) -o $@ $< $(MPCFLAGS)

$(BUILD)/mpy.raw: $(PY_OBJS) $(PORT_OBJS) $(MPBUILD)/frozen_content.o
	$(LN) -o $@ $(LNFLAGS) --list-file=$(BUILD)/mpy.map $^

clean:
	rm -rf $(BUILD)
