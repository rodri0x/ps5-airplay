PS5_PAYLOAD_SDK ?= /opt/ps5-payload-sdk

# Homebrew's LLVM is not on the PATH; elsewhere the SDK finds llvm-config itself.
ifeq ($(origin LLVM_CONFIG),undefined)
ifneq ($(wildcard /opt/homebrew/opt/llvm@18/bin/llvm-config),)
export LLVM_CONFIG := /opt/homebrew/opt/llvm@18/bin/llvm-config
endif
endif

SDK_MK := $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk
ifeq ($(wildcard $(SDK_MK)),)
$(error PS5_PAYLOAD_SDK is not a ps5-payload-sdk tree: $(PS5_PAYLOAD_SDK))
endif
include $(SDK_MK)

CC := $(PS5_PAYLOAD_SDK)/bin/prospero-clang
CXX := $(PS5_PAYLOAD_SDK)/bin/prospero-clang++
AR := $(PS5_PAYLOAD_SDK)/bin/prospero-ar
STRIP := $(PS5_PAYLOAD_SDK)/bin/prospero-strip
TARGET := $(PS5_PAYLOAD_SDK)/target

VERSION := $(strip $(shell cat VERSION))

BUILD := build
DIST := dist
FFMPEG_DIR := third_party/ffmpeg
ALAC_DIR := third_party/alac
FDK_DIR := third_party/fdk-aac

ELF := $(BUILD)/ps5airplay.elf
INSTALLER := $(DIST)/airplay-installer.elf
HOST_ELF := $(BUILD)/display_host.elf

CPPFLAGS := -Iinclude -I$(TARGET)/include -Isrc -Isrc/crypto -Isrc/playfair \
	-Ithird_party/tweetnacl -Ithird_party/websrv -I$(BUILD) \
	-DAIRPLAY_VERSION=\"$(VERSION)\"
WARNINGS := -Wall -Wextra -Werror -Wno-error=unused-function \
	-Wno-error=unused-parameter -Wno-error=unused-variable
CFLAGS := -std=c11 -Os $(WARNINGS) -Wno-error=sometimes-uninitialized \
	-ffunction-sections -fdata-sections
CXXFLAGS := -std=c++17 -Os $(WARNINGS) -ffunction-sections -fdata-sections
DEPFLAGS := -MMD -MP
LDFLAGS := -Wl,--gc-sections

AIRPLAY_SRCS := src/airplay.c src/airplay_audio.c src/airplay_crypto.c \
	src/airplay_mirror.c src/airplay_pairing.c src/airplay_playout.c \
	src/airplay_srp.c src/airplay_timing.c src/bplist.c src/control.c \
	src/display_carrier.c src/mdns.c src/notify.c src/ps5_audio.c \
	src/ps5_video.c src/receiver_log.c src/rtsp.c src/update_check.c \
	src/crypto/aes.c \
	src/playfair/hand_garble.c src/playfair/modified_md5.c \
	src/playfair/omg_hax.c src/playfair/playfair.c src/playfair/sap_hash.c
AIRPLAY_CPP_SRCS := src/h264_decoder.cpp src/audio_decoder.cpp
THIRD_PARTY_OBJS := $(BUILD)/third_party/tweetnacl/tweetnacl.o \
	$(BUILD)/third_party/websrv/pt.o $(BUILD)/third_party/websrv/elfldr.o
HOST_ASSET_C := $(BUILD)/display_host_asset.c
AIRPLAY_OBJS := $(patsubst %.c,$(BUILD)/%.o,$(AIRPLAY_SRCS)) \
	$(patsubst %.cpp,$(BUILD)/%.o,$(AIRPLAY_CPP_SRCS)) \
	$(THIRD_PARTY_OBJS) $(BUILD)/display_host_asset.o
AIRPLAY_LIBS := -lSceNet -lSceVideoOut -lSceAudioOut -lSceSystemService \
	-lSceUserService -lSceSsl -lSceHttp2 -lpthread \
	$(FFMPEG_DIR)/lib/libavcodec.a $(FFMPEG_DIR)/lib/libavutil.a -lc++ -lc++abi \
	$(ALAC_DIR)/libalac.a $(FDK_DIR)/libfdk-decoder.a
CODEC_CPPFLAGS := -I$(FFMPEG_DIR)/include -I$(ALAC_DIR)/codec \
	-I$(FDK_DIR)/libAACdec/include -I$(FDK_DIR)/libSYS/include

INSTALLER_SRCS := src/installer.c src/installer_http.c src/app_install.c src/notify.c
INSTALLER_LIBS := -lSceNet -lSceNetCtl -lSceIpmi -lSceAppInstUtil \
	-lSceSystemService -lSceUserService
ASSET_C := $(BUILD)/embedded_assets.c
ASSETS := assets/param.json assets/airplay.appcache assets/installer.html \
	assets/app/index.html assets/icon0.png

.PHONY: all clean test probes deploy

all: $(ELF) $(INSTALLER)

$(BUILD) $(DIST):
	mkdir -p $@

$(ALAC_DIR)/libalac.a:
	$(MAKE) -C $(ALAC_DIR) CC=$(CC) CXX=$(CXX) AR=$(AR) \
		CFLAGS='-O2 -fPIC' CXXFLAGS='-O2 -fPIC' libalac.a

$(FDK_DIR)/libfdk-decoder.a:
	$(MAKE) -f Makefile.payload -C $(FDK_DIR) CC=$(CC) CXX=$(CXX) AR=$(AR) \
		CXXFLAGS='-O2 -fPIC' libfdk-decoder.a

# Third-party sources are built as shipped.  These rules come first because
# GNU make 3.81 (macOS) takes the first pattern rule that matches.
$(BUILD)/third_party/%.o: third_party/%.c
	mkdir -p $(dir $@)
	$(CC) -I$(TARGET)/include -Ithird_party/tweetnacl -Ithird_party/websrv -Os -w -c -o $@ $<

$(BUILD)/%.o: %.c VERSION
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.cpp VERSION
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CODEC_CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c -o $@ $<

$(HOST_ELF): src/display_host.c src/receiver_log.c src/receiver_log.h src/ps5_sce.h include/airplay_config.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ src/display_host.c src/receiver_log.c \
		-lSceSystemService -lSceAudioOut -lSceNet -lpthread
	$(STRIP) $@

$(HOST_ASSET_C): tools/embed_assets.py $(HOST_ELF)
	python3 tools/embed_assets.py --output $@ --asset display_host_elf=$(HOST_ELF)

$(BUILD)/display_host_asset.o: $(HOST_ASSET_C)
	$(CC) -Os -c -o $@ $<

$(ELF): $(AIRPLAY_OBJS) $(ALAC_DIR)/libalac.a $(FDK_DIR)/libfdk-decoder.a
	$(CXX) $(LDFLAGS) -o $@ $(AIRPLAY_OBJS) $(AIRPLAY_LIBS)
	$(STRIP) $@

$(ASSET_C): tools/embed_assets.py $(ASSETS) $(ELF)
	python3 tools/embed_assets.py --output $@ \
		--asset param_json=assets/param.json \
		--asset cache_manifest=assets/airplay.appcache \
		--asset installer_html=assets/installer.html \
		--asset app_html=assets/app/index.html \
		--asset icon0_png=assets/icon0.png \
		--asset airplay_elf=$(ELF)

$(INSTALLER): $(INSTALLER_SRCS) $(ASSET_C) VERSION src/*.h include/*.h | $(DIST)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $(INSTALLER_SRCS) $(ASSET_C) $(INSTALLER_LIBS)
	$(STRIP) $@

-include $(AIRPLAY_OBJS:.o=.d)

test:
	python3 -m unittest discover -s tests -v

# Diagnostic payloads: the log reader, a receiver stopper and a test pattern.
probes: $(BUILD)/receiver_log_probe.elf $(BUILD)/stop_receiver_probe.elf $(BUILD)/video_pattern.elf

$(BUILD)/%_probe.elf: tests/%_probe.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< -lSceNet
	$(STRIP) $@

$(BUILD)/video_pattern.elf: tests/video_pattern.c src/ps5_video.c src/receiver_log.c | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $^ -lSceVideoOut -lSceSystemService -lSceNet -lpthread
	$(STRIP) $@

deploy: $(INSTALLER)
	$(PS5_DEPLOY) -h $${PS5_HOST:-ps5} -p $${PS5_PORT:-9021} $(INSTALLER)

clean:
	rm -rf $(BUILD) $(DIST)
