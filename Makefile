VERSION=0.1.0
IMAGESIZE = 524288
DEFAULT_CONFIG_LOCATION = 454656
CONFIG_LOCATION = 458752
# Startup config baked into the image, also restored by a factory reset.
# The committed config.txt is the generic default; point CONFIG at your own
# file for a site-specific build: make CONFIG=../site.cfg
CONFIG ?= config.txt

ifeq ($(origin CC),default)
CC = sdcc
endif
CC_FLAGS = -mmcs51 -I. -Iuip
ASM ?= sdas8051
AFLAGS= -plosgff

SUBDIRS := tools
SUBDIRSCLEAN=$(addsuffix clean,$(SUBDIRS))

ifeq ($(MACHINE),)
	MACHINE:= $(shell grep "^\s*#define MACHINE_" machine.h | sed "s/^\s*#define MACHINE_//")
else
	CC_FLAGS += -DMACHINE_$(MACHINE)
endif

ifeq ($(CI),1)
	CC_FLAGS += --Werror
endif

BUILDDIR = output/$(MACHINE)
VERSION_HEADER := version.h

GIT_VERSION := $(shell git rev-parse --short HEAD)
ifeq ($(shell git status --porcelain --untracked-files=no),)
else
	GIT_VERSION := $(GIT_VERSION)-dirty
endif

VERSION_EXTENSION = v$(VERSION)-$(GIT_VERSION)
FILENAME_EXTENSION = $(VERSION_EXTENSION)-$(MACHINE)

# Deterministic build date: honor SOURCE_DATE_EPOCH, else the HEAD commit date,
# else wall-clock (no-git fallback). Keeps same-commit builds byte-identical
# (BUILD_DATE is baked into the image and covered by the trailing CRC).
SOURCE_DATE_EPOCH ?= $(shell git show -s --format=%ct HEAD 2>/dev/null)
ifeq ($(SOURCE_DATE_EPOCH),)
BUILD_DATE := $(shell date +"%Y-%m-%d %H:%M:%S")
else
BUILD_DATE := $(shell date -u -d @$(SOURCE_DATE_EPOCH) +"%Y-%m-%d %H:%M:%S" 2>/dev/null \
	|| date -u -r $(SOURCE_DATE_EPOCH) +"%Y-%m-%d %H:%M:%S")
endif

all: create_build_dir $(VERSION_HEADER) $(SUBDIRS) $(BUILDDIR)/rtl-swos-$(FILENAME_EXTENSION).bin

create_build_dir:
	mkdir -p "$(BUILDDIR)"
	mkdir -p "$(BUILDDIR)/uip"

# Keep machine.c in first position to fail immediately on invalid $MACHINE value
SRCS = \
	machine.c \
	machine_init.c \
	cli.c \
	cli_act.c \
	dbgcmd.c \
	cmd_editor.c \
	console.c \
	dhcp.c \
	main.c \
	boot.c \
	sfp.c \
	swcfg.c \
	lacp.c \
	runcfg.c \
	show.c \
	tcp_app.c \
	telnetd.c \
	tftp.c \
	syslog.c \
	dns.c \
	ntp.c \
	totp.c \
	ping.c \
	bank4.c \
	log.c \
	log_links.c \
	udp_apps.c

# RTL837x
SRCS += \
	rtl837x_bandwidth.c \
	rtl837x_flash.c \
	rtl837x_igmp.c \
	rtl837x_init.c \
	rtl837x_leds.c \
	rtl837x_phy.c \
	rtl837x_pins.c\
	rtl837x_port.c \
	rtl837x_stp.c
SRCS += \
	uip/uip.c \
	uip/uip_arp.c

OBJS = ${SRCS:%.c=$(BUILDDIR)/%.rel}
DEPS := ${SRCS:%.c=$(BUILDDIR)/%.d}

$(VERSION_HEADER):
	@printf '%s\n' "#ifndef VERSION_H" "#define VERSION_H" \
		"#define VERSION_SW \"$(VERSION_EXTENSION)\"" \
		"#define BUILD_DATE \"$(BUILD_DATE)\"" \
		"#endif" > $(VERSION_HEADER).tmp
	@cmp -s $(VERSION_HEADER).tmp $(VERSION_HEADER) && rm $(VERSION_HEADER).tmp \
		|| mv $(VERSION_HEADER).tmp $(VERSION_HEADER)

$(SUBDIRS):
	$(MAKE) -C $@

clean: $(SUBDIRSCLEAN)
	-rm -f $(VERSION_HEADER)
	-if [ -d $(BUILDDIR) ]; then find $(BUILDDIR) -type f ! -name "*.bin" -delete; fi

distclean: $(SUBDIRSCLEAN)
	-rm -f $(VERSION_HEADER)
	-rm -rf $(BUILDDIR)

$(SUBDIRSCLEAN):
	$(MAKE) -C $(@:clean=) clean

$(BUILDDIR)/%.rel: %.c | create_build_dir
	$(CC) -MMD $(CC_FLAGS) -o $@ -c $<

$(BUILDDIR)/%.rel: %.asm | create_build_dir
	${ASM} ${AFLAGS} -o $@ $<
#	mv -f $(addprefix $(basename $^), .lst .rel .sym) .

$(BUILDDIR)/rtl-swos.ihx: $(OBJS) $(BUILDDIR)/crtbank.rel $(BUILDDIR)/crc16.rel
	$(CC) $(CC_FLAGS) -Wl-bHOME=0x00000 -Wl-bBANK1=0x14000 -Wl-bBANK2=0x24000 -Wl-bBANK3=0x34000 -Wl-bBANK4=0x44000 -Wl-r -o $@ $^

# Ordinary __xdata must stay below 0x4000: the startup XRAM clear does
# not reach above it (see XRAM_LOW_LIMIT in rtl837x_common.h).
$(BUILDDIR)/rtl-swos.img: $(BUILDDIR)/rtl-swos.ihx
	@end=$$(awk '/ s_XISEG /{s=strtonum("0x"$$2)} / l_XISEG /{l=strtonum("0x"$$2)} END{printf "%d", s+l}' $(BUILDDIR)/rtl-swos.map); \
	if [ $$end -gt 16384 ]; then \
		echo "ERROR: xdata ends at $$(printf 0x%x $$end), above XRAM_LOW_LIMIT 0x4000"; exit 1; \
	else echo "xdata ends at $$(printf 0x%x $$end) (limit 0x4000)"; fi
	objcopy --input-target=ihex -O binary $< $@

# Always re-assembled (cheap): which file CONFIG names can change between
# builds without any timestamp noticing.
$(BUILDDIR)/rtl-swos-$(FILENAME_EXTENSION).bin: $(BUILDDIR)/rtl-swos.img $(CONFIG) FORCE | tools
	if [ -e $@ ]; then rm $@; fi
	tools/output/imagebuilder -i $< $@
	tools/output/fileadder -a $(DEFAULT_CONFIG_LOCATION) -s $(IMAGESIZE) -d $(CONFIG) $@
	tools/output/fileadder -a $(CONFIG_LOCATION) -s $(IMAGESIZE) -d $(CONFIG) $@
	tools/output/crc_calculator -u $@
	ln -sf $(MACHINE)/rtl-swos-$(FILENAME_EXTENSION).bin output/rtl-swos.bin

.PHONY: clean distclean all $(SUBDIRS) $(SUBDIRSCLEAN) $(VERSION_HEADER) create_build_dir FORCE
FORCE:

.PHONY:
machine_check:
	@mkdir -p $(BUILDDIR)/tmp
	@set -eo pipefail; \
	for MACHINE in `grep -E '^[[:space:]]*(//[[:space:]]*)?#define MACHINE_' machine.h | sed -E 's%^[[:space:]]*(//[[:space:]]*)?#define MACHINE_%%' | awk '{print $$1}' | sort -u`; \
	do \
	echo "Checking $${MACHINE}"; \
	$(CC) $(CC_FLAGS) -DMACHINE_$${MACHINE} -MMD -o $(BUILDDIR)/tmp/machine_check -c machine.c; \
	$(CC) $(CC_FLAGS) -DMACHINE_$${MACHINE} -MMD -o $(BUILDDIR)/tmp/machine_check -c machine_init.c; \
	done
	@rm -rf $(BUILDDIR)/tmp

-include $(DEPS)
