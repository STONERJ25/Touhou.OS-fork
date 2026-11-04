# The system builds itself: libgap, kappabox, mmake and mcc, from the sources that sit next to this file (/ScarletDevilMansion/src),
# with the compiler, linker and archiver of the system (mcc, mld, mar) and mmake itself.  docs/SELFHOST.md has the story.
#
#   cd /ScarletDevilMansion/src && make              everything into $(OUT) (default /tmp/build)
#   make OUT=/tmp/b2 libgap kappabox                 only those, somewhere else
#   make install DESTDIR=/tmp/newroot                copy the programs to $(DESTDIR)/bin
#   make clean
#
# The libraries other than libgap (libkappa, libfairy, ...) and crt0.o come from the system root: rebuilding them is the same
# recipe (see the host build, tools/mcc_userland.py), they are just not needed to rebuild the command-line system.

SRC    := $(patsubst %/,%,$(dir $(abspath $(firstword $(MAKEFILE_LIST)))))
ROOT   ?= /ScarletDevilMansion
OUT    ?= /tmp/build
DESTDIR ?=
BINDIR ?= /bin

CC      = mcc
AR      = mar
MAS    ?= /bin/mas

# every library of the system root except libgap, which is rebuilt here: -lkappa -lfairy ...
SYSLIBS := $(patsubst $(ROOT)/lib/lib%.a,-l%,$(filter-out $(ROOT)/lib/libgap.a,$(wildcard $(ROOT)/lib/lib*.a)))

# fpconv.c and math.c use unsigned __int128, which mcc does not have: their objects come out of the system root's libgap.a (built by the host's
# compiler), as setjmp.o does without mas
PREBUILT := fpconv math
LIBC_SRC := $(filter-out $(patsubst %,$(SRC)/libc/%.c,$(PREBUILT)),$(wildcard $(SRC)/libc/*.c))
LIBC_OBJ := $(patsubst $(SRC)/libc/%.c,$(OUT)/obj/libc/%.o,$(LIBC_SRC))
ASM_OBJ  := $(OUT)/obj/libc/crt0.o $(OUT)/obj/libc/setjmp.o
PRE_OBJ  := $(patsubst %,$(OUT)/obj/libc/%.o,$(PREBUILT))
KB_SRC   := $(wildcard $(SRC)/kappabox/*.c)
KB_OBJ   := $(patsubst $(SRC)/kappabox/%.c,$(OUT)/obj/kappabox/%.o,$(KB_SRC)) $(OUT)/obj/kappabox/applet_table.o

.PHONY: all libgap kappabox mmake mcc mas install clean check
all: libgap kappabox mmake mcc mas

libgap: $(OUT)/lib/libgap.a
kappabox: $(OUT)/bin/kappabox
mmake: $(OUT)/bin/mmake
mcc: $(OUT)/bin/mcc
mas: $(OUT)/bin/mas

# ---- libgap: the C sources with mcc, the two assembly sources with mas (or, on a system without mas, the objects of the system root)
$(OUT)/obj/libc/%.o: $(SRC)/libc/%.c
	@mkdir -p $(@D)
	$(CC) -c -I $(SRC)/libc $< -o $@

ifneq ($(wildcard $(MAS)),)
$(OUT)/obj/libc/crt0.o: $(SRC)/libc/crt0.asm
	@mkdir -p $(@D)
	$(MAS) -o $@ $<
$(OUT)/obj/libc/setjmp.o: $(SRC)/libc/setjmp.asm
	@mkdir -p $(@D)
	$(MAS) -o $@ $<
else
$(OUT)/obj/libc/crt0.o: $(ROOT)/lib/crt0.o
	@mkdir -p $(@D)
	cp $< $@
$(OUT)/obj/libc/setjmp.o: $(ROOT)/lib/libgap.a
	@mkdir -p $(@D)
	cd $(@D) && $(AR) x $< setjmp.asm.o && mv setjmp.asm.o setjmp.o
endif

$(PRE_OBJ): $(OUT)/obj/libc/%.o: $(ROOT)/lib/libgap.a
	@mkdir -p $(@D)
	cd $(@D) && $(AR) x $< $*.c.o && mv $*.c.o $*.o

$(OUT)/lib/libgap.a: $(LIBC_OBJ) $(PRE_OBJ) $(OUT)/obj/libc/setjmp.o
	@mkdir -p $(@D)
	rm -f $@
	$(AR) rcs $@ $^

# ---- kappabox: one object per source, plus the table of applets that the sources announce with APPLET(name)
$(OUT)/obj/kappabox/applet_table.c: $(KB_SRC) $(SRC)/gen-applets.awk
	@mkdir -p $(@D)
	grep -h '^APPLET(' $(SRC)/kappabox/*.c | sed 's/^APPLET(\([A-Za-z0-9_]*\)).*/\1/' | sort -u | awk -f $(SRC)/gen-applets.awk > $@

$(OUT)/obj/kappabox/applet_table.o: $(OUT)/obj/kappabox/applet_table.c
	$(CC) -c $< -o $@

$(OUT)/obj/kappabox/%.o: $(SRC)/kappabox/%.c
	@mkdir -p $(@D)
	$(CC) -c -I $(SRC)/kappabox $< -o $@

$(OUT)/bin/kappabox: $(KB_OBJ) $(OUT)/lib/libgap.a $(OUT)/obj/libc/crt0.o
	@mkdir -p $(@D)
	$(CC) -nostdlib $(OUT)/obj/libc/crt0.o $(KB_OBJ) -L $(OUT)/lib $(SYSLIBS) $(OUT)/lib/libgap.a -o $@

# ---- the build tools themselves
$(OUT)/bin/mmake: $(SRC)/mmake/mmake.c $(OUT)/lib/libgap.a $(OUT)/obj/libc/crt0.o
	@mkdir -p $(@D)
	$(CC) -nostdlib $(OUT)/obj/libc/crt0.o $< $(OUT)/lib/libgap.a -o $@

$(OUT)/bin/mcc: $(wildcard $(SRC)/mcc/*.c $(SRC)/mcc/*.h $(SRC)/mcc/*.inc) $(OUT)/lib/libgap.a $(OUT)/obj/libc/crt0.o
	@mkdir -p $(@D)
	$(CC) -nostdlib $(OUT)/obj/libc/crt0.o $(SRC)/mcc/main.c $(OUT)/lib/libgap.a -o $@

$(OUT)/bin/mas: $(wildcard $(SRC)/mas/*.c $(SRC)/mas/*.inc) $(OUT)/lib/libgap.a $(OUT)/obj/libc/crt0.o
	@mkdir -p $(@D)
	$(CC) -nostdlib $(OUT)/obj/libc/crt0.o $(SRC)/mas/mas.c $(OUT)/lib/libgap.a -o $@

install: all
	mkdir -p $(DESTDIR)$(BINDIR)
	cp $(OUT)/bin/kappabox $(OUT)/bin/mmake $(OUT)/bin/mcc $(OUT)/bin/mas $(DESTDIR)$(BINDIR)
	chmod 755 $(DESTDIR)$(BINDIR)/kappabox $(DESTDIR)$(BINDIR)/mmake $(DESTDIR)$(BINDIR)/mcc $(DESTDIR)$(BINDIR)/mas

check: kappabox
	$(OUT)/bin/kappabox echo "kappabox rebuilt: $$($(OUT)/bin/kappabox uname -s)"

clean:
	rm -rf $(OUT)
