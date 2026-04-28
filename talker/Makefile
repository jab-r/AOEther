CC      ?= cc
CXX     ?= c++
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -D_GNU_SOURCE
LDLIBS  ?= -lm -lasound

BUILD     := build
INCLUDES  := -Isrc -I../common
SRCS      := \
    src/talker.c \
    src/audio_source_test.c \
    src/audio_source_wav.c \
    src/audio_source_alsa.c \
    src/audio_source_dsd.c \
    src/audio_source_dsf.c \
    src/audio_source_dff.c \
    ../common/packet.c \
    ../common/avtp.c \
    ../common/dop.c \
    ../common/rtp.c \
    ../common/sdp.c \
    ../common/sap.c \
    ../common/ptp_pmc.c \
    ../common/avdecc.c

# AVDECC entity (M7 Phase B) is optional. See receiver/Makefile for
# details — same scheme applies here.
AVDECC_LIB := ../avdecc/build/libaoether_avdecc.a
HAVE_AVDECC := $(shell test -f $(AVDECC_LIB) && echo 1)
ifeq ($(HAVE_AVDECC),1)
CFLAGS  += -DAOETHER_HAVE_AVDECC
LDLIBS  += $(AVDECC_LIB) -lpcap -lstdc++ -lpthread
LINK    := $(CXX)
else
LINK    := $(CC)
endif

.PHONY: all clean
all: $(BUILD)/talker

$(BUILD)/talker: $(SRCS) src/audio_source.h ../common/packet.h ../common/avtp.h ../common/dop.h ../common/rtp.h ../common/sdp.h ../common/sap.h ../common/ptp_pmc.h ../common/avdecc.h | $(BUILD)
	$(LINK) $(CFLAGS) $(INCLUDES) $(SRCS) -o $@ $(LDLIBS)

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
