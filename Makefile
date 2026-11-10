#
# Makefile for the BMAtariST kernel image.
#
# Run ./make_all.sh once first; it builds circle-stdlib and then this.
# After that "make" rebuilds the kernel image for the board circle-stdlib is
# built for: kernel7.img (Pi 2), kernel8-32.img (Pi 3), kernel8-rpi4.img
# (Pi 4, 64-bit) or kernel_2712.img (Pi 5, 64-bit).
# ./build_sdcard.sh stages everything for the SD card in build/sdcard.
#

ROOT := $(CURDIR)

ifeq ($(wildcard third_party/circle-stdlib/Config.mk),)
$(error circle-stdlib is not configured. Run ./make_all.sh first)
endif

include third_party/circle-stdlib/Config.mk
# RASPPI (the board) is in Circle's own Config.mk, which circle-stdlib's
# configure writes and Circle's Rules.mk reads; it's needed here first to
# pick the board's display and sound code.
include $(CIRCLEHOME)/Config.mk

BMATARIST_VERSION ?= $(shell tools/version.sh --semver)
BMATARIST_GIT ?= $(shell tools/version.sh --git)
HATARI := third_party/hatari/src
ZLIB := third_party/zlib
# "v2.6.1-579-g90949cfa": the release the submodule is after, and its commit.
HATARI_VERSION := $(shell git -C third_party/hatari describe --tags 2>/dev/null || echo unknown)

include src/port/hatari.mk

BUILD := build
GEN := $(BUILD)/gen
HATARI_OBJS := $(addprefix $(BUILD)/hatari/,$(HATARI_CORE_SRCS:.c=.o)) \
               $(addprefix $(BUILD)/hatari/cpu/,$(HATARI_CPU_SRCS:.c=.o)) \
               $(addprefix $(GEN)/,$(HATARI_CPUGEN_SRCS:.c=.o) $(HATARI_CPUDEFS_SRC:.c=.o)) \
               $(addprefix $(BUILD)/hatari/falcon/,$(HATARI_FALCON_SRCS:.c=.o)) \
               $(addprefix $(BUILD)/hatari/debug/,$(HATARI_DEBUG_SRCS:.c=.o)) \
               $(addprefix $(BUILD)/zlib/,$(ZLIB_SRCS:.c=.o))
HATARI_LIB := $(BUILD)/libhatari.a

OBJS = src/main.o src/kernel.o src/logging.o src/options.o \
       src/port/platform.o src/port/menu.o src/port/osd.o src/port/posix_extra.o \
       src/port/fswrap.o src/port/bigalloc.o src/port/bigalloc_size.o \
       $(addprefix src/port/,$(HATARI_PORT_SRCS:.c=.o)) \
       src/network.o src/network_time_sync.o \
       src/webui/webui.o src/webui/webui_http.o src/webui/webui_fs.o \
       src/webui/webui_machine.o src/webui/webui_assets.o \
       src/sdcard/sd_fs_fatfs.o src/sdcard/sd_fs_file.o

# Display and sound. The Pi 2 and 3 use the VideoCore through VCHIQ
# (dispmanx, from BMC64); the Pi 4 and 5 use Circle's framebuffer and HDMI
# sound drivers (src/av/, shared by both; see docs/PI4.md and docs/PI5.md).
ifneq ($(filter 4 5,$(RASPPI)),)
OBJS += src/av/video.o src/av/sound.o src/av/hdmisound.o src/av/scaler.o \
        src/kms/common/filter.o src/kms/common/modes.o \
        src/v3d/gpu.o src/v3d/draw.o src/v3d/crt.o \
        src/v3d/shaders.o
# The direct display driver: the Pi 4's (src/kms/pi4/) or the Pi 5's
# (src/kms/pi5/), with shared filter kernels and mode calculations.
ifeq ($(RASPPI),4)
OBJS += src/kms/pi4/dlist.o src/kms/pi4/hvs.o src/kms/pi4/display.o \
        src/kms/pi4/phy.o src/kms/pi4/modeset.o
else
OBJS += src/kms/pi5/dlist.o src/kms/pi5/hvs.o src/kms/pi5/display.o \
        src/kms/pi5/phy.o src/kms/pi5/modeset.o
endif
# Smooth scaling and the CRT shader are drawn by the ARM: -O3 lets GCC turn
# their per-pixel loops into NEON code (tools/pi4scaler tests the same).
src/av/scaler.o: OPTIMIZE = -O3
else
OBJS += src/fbl.o src/crt_pi_idx.o src/crt_pi_rgb.o src/vchiqsound.o \
        src/vchiqsoundbasedevice.o
endif

include $(CIRCLEHOME)/Rules.mk

INCLUDE := $(CIRCLE_STDLIB_INCLUDES) $(INCLUDE) -I $(CIRCLEHOME)/addon/fatfs

# See src/port/fswrap.c
LDFLAGS += --wrap=stat --wrap=tmpfile
# See src/port/bigalloc.c
LDFLAGS += --wrap=malloc --wrap=free --wrap=calloc --wrap=realloc

VERSION_DEFINES = -DBMATARIST_VERSION=\"$(BMATARIST_VERSION)\" \
                  -DBMATARIST_GIT=\"$(BMATARIST_GIT)\" -DHATARI_GIT=\"$(HATARI_VERSION)\"
CFLAGS += $(VERSION_DEFINES)
CPPFLAGS += $(VERSION_DEFINES)

HATARI_INCLUDES = -I src/port -I $(HATARI)/includes -I $(HATARI)/debug -I $(HATARI)/falcon \
                  -I $(HATARI)/cpu -I $(HATARI)/cpu/softfloat -I $(HATARI) -I $(GEN) -I $(ZLIB)

# The port's C files include Hatari's headers.
src/port/%.o src/port/%.d: CFLAGS += $(HATARI_INCLUDES) -std=gnu11 \
	-include src/port/compat/hatari_prefix.h
$(addprefix src/port/,$(HATARI_PORT_SRCS:.c=.o) platform.o menu.o): $(GEN)/cputbl.h

# Hatari itself (and zlib): compiled into build/ so the submodules stay
# clean.
HATARI_FLAGS = $(HATARI_CFLAGS) $(HATARI_INCLUDES) -include src/port/compat/hatari_prefix.h

# The CPU core's generated sources, as Hatari's build makes them for a
# cross compile (src/cpu/CMakeLists.txt): build68k and gencpu are built for
# this computer, with its compiler.
HOSTCC ?= cc

$(GEN)/build68k: $(HATARI)/cpu/build68k.c $(HATARI)/cpu/writelog.c
	@mkdir -p $(GEN)
	@echo "  HOSTCC $@"
	@$(HOSTCC) -I $(HATARI)/cpu -I $(HATARI)/includes -I src/port -o $@ $^

$(GEN)/cpudefs.c: $(GEN)/build68k $(HATARI)/cpu/table68k
	@echo "  GEN   $@"
	@$(GEN)/build68k < $(HATARI)/cpu/table68k > $@

$(GEN)/gencpu: $(GEN)/cpudefs.c $(HATARI)/cpu/gencpu.c $(HATARI)/cpu/readcpu.c
	@echo "  HOSTCC $@"
	@$(HOSTCC) -I $(HATARI)/cpu -I $(HATARI)/includes -I src/port -o $@ $^

$(GEN)/cpustbl.c: $(GEN)/gencpu
	@echo "  GEN   $(GEN)/cpuemu_*.c"
	@cd $(GEN) && ./gencpu > /dev/null

$(addprefix $(GEN)/,$(filter-out cpustbl.c,$(HATARI_CPUGEN_SRCS))) $(GEN)/cputbl.h: $(GEN)/cpustbl.c

# A static pattern rule: Circle's own %.o: %.c (Rules.mk) would otherwise be
# used for these, which are .c files next to their .o (make 3.81, macOS's,
# takes the first rule that matches).
HATARI_GEN_OBJS := $(addprefix $(GEN)/,$(HATARI_CPUGEN_SRCS:.c=.o) $(HATARI_CPUDEFS_SRC:.c=.o))
$(HATARI_GEN_OBJS): $(GEN)/%.o: $(GEN)/%.c
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) $(HATARI_FLAGS) -c -o $@ $<

$(BUILD)/hatari/%.o: $(HATARI)/%.c $(GEN)/cputbl.h
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) $(HATARI_FLAGS) -c -o $@ $<

$(BUILD)/zlib/%.o: $(ZLIB)/%.c
	@mkdir -p $(dir $@)
	@echo "  CC    $<"
	@$(CC) $(CFLAGS) $(ZLIB_CFLAGS) -c -o $@ $<

$(HATARI_LIB): $(HATARI_OBJS)
	@echo "  AR    $@"
	@rm -f $@
	@$(AR) cr $@ $(HATARI_OBJS)

# The web UI's pages are compiled in. Regenerate them when they change.
WEBUI_ASSETS = $(shell find src/webui/assets -type f)
src/webui/webui_assets.c: $(WEBUI_ASSETS) tools/gen_webui_assets.py
	@echo "  GEN   $@"
	@python3 tools/gen_webui_assets.py

$(TARGET).img: $(HATARI_LIB)

ifneq ($(filter 4 5,$(RASPPI)),)
VIDEOCORE_LIBS :=
else
VIDEOCORE_LIBS := \
	$(CIRCLEHOME)/addon/vc4/vchiq/libvchiq.a \
	$(CIRCLEHOME)/addon/vc4/interface/bcm_host/libbcm_host.a \
	$(CIRCLEHOME)/addon/vc4/interface/khronos/libkhrn_client.a \
	$(CIRCLEHOME)/addon/vc4/interface/vmcs_host/libvmcs_host.a \
	$(CIRCLEHOME)/addon/vc4/interface/vcos/libvcos.a \
	$(CIRCLEHOME)/addon/linux/liblinuxemu.a
endif

LIBS := $(HATARI_LIB) \
	$(CIRCLE_STDLIB_INSTALL_DIR)/lib/libm.a \
	$(CIRCLE_STDLIB_INSTALL_DIR)/lib/libc.a \
	$(CIRCLE_STDLIB_INSTALL_DIR)/lib/libcirclenewlib.a \
	$(CIRCLEHOME)/addon/SDCard/libsdcard.a \
	$(CIRCLEHOME)/lib/usb/libusb.a \
	$(CIRCLEHOME)/lib/input/libinput.a \
	$(CIRCLEHOME)/addon/fatfs/libfatfs.a \
	$(CIRCLEHOME)/lib/fs/libfs.a \
	$(CIRCLEHOME)/lib/net/libnet.a \
	$(CIRCLEHOME)/addon/wlan/hostap/wpa_supplicant/libwpa_supplicant.a \
	$(CIRCLEHOME)/addon/wlan/libwlan.a \
	$(VIDEOCORE_LIBS) \
	$(CIRCLEHOME)/lib/sound/libsound.a \
	$(CIRCLEHOME)/lib/sched/libsched.a \
	$(CIRCLEHOME)/lib/libcircle.a

EXTRACLEAN += $(OBJS) src/*.o src/port/*.d src/*.d src/webui/*.d src/sdcard/*.d \
              src/av/*.o src/av/*.d src/kms/common/*.o src/kms/common/*.d \
              src/kms/pi4/*.o src/kms/pi4/*.d \
              src/kms/pi5/*.o src/kms/pi5/*.d src/v3d/*.o src/v3d/*.d
.PHONY: distclean

distclean: clean
	rm -rf $(BUILD)

-include $(DEPS)
