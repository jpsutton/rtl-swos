# Host unit-test harness for rtl-swos firmware logic.
#
# Compiles selected firmware translation units with host gcc + AddressSanitizer,
# force-including sdcc_shim.h to erase 8051 keywords, and links them against
# mocked hardware edges (support.c). No SDCC, no flashing, no hardware.
#
#   make        build + run all tests
#   make run    run tests
#   make clean

CC       = gcc
SRCROOT  = ..
# -include shim first so __xdata/__code/__banked vanish before any firmware header.
CFLAGS   = -std=gnu11 -g -O1 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-unknown-pragmas \
           -Wno-pointer-sign \
           -DSWOS_HOST_TEST \
           -fsanitize=address,undefined -fno-omit-frame-pointer \
           -include sdcc_shim.h -Istub -I. -I$(SRCROOT) -I$(SRCROOT)/uip
ifeq ($(findstring clang,$(shell $(CC) --version 2>/dev/null)),)
CFLAGS  += -Wno-builtin-declaration-mismatch
endif
LDFLAGS  = -fsanitize=address,undefined

BUILDDIR = build

# One binary per subsystem under test.
TESTS = $(BUILDDIR)/test_cmd_editor \
        $(BUILDDIR)/test_cli \
        $(BUILDDIR)/test_port_tables

# Every test links the same environment: the register mock plus the two
# environment files, with the command editor, the CLI engine, the
# switch-config state model and the port driver compiled UNMODIFIED from
# the firmware tree. stub/ stands in for the header a firmware build
# generates.
CORE_ENV = support.c hw_mock.c env_tables.c env_cli.c \
           $(SRCROOT)/cmd_editor.c $(SRCROOT)/cli.c $(SRCROOT)/cli_act.c $(SRCROOT)/swcfg.c \
           $(SRCROOT)/runcfg.c $(SRCROOT)/show.c \
           $(SRCROOT)/rtl837x_port.c

all: run

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/%: %.c $(CORE_ENV) hw_mock.h support.h | $(BUILDDIR)
	$(CC) $(CFLAGS) $(filter %.c,$^) -o $@ $(LDFLAGS)

run: $(TESTS)
	@fail=0; for t in $(TESTS); do \
		echo "=== $$t ==="; \
		$$t || fail=1; \
		echo; \
	done; \
	if [ $$fail -ne 0 ]; then echo "TESTS FAILED"; exit 1; else echo "ALL TESTS PASSED"; fi

clean:
	rm -rf $(BUILDDIR)

.PHONY: all run clean
