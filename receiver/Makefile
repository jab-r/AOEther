CC      ?= cc
CXX     ?= c++
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -D_GNU_SOURCE
LDLIBS  ?= -lasound

BUILD     := build
INCLUDES  := -Isrc -I../common
SRCS      := \
    src/receiver.c \
    ../common/packet.c \
    ../common/avtp.c \
    ../common/rtp.c \
    ../common/sdp.c \
    ../common/sap.c \
    ../common/mdns.c \
    ../common/avdecc.c

# mDNS-SD advertisement (M7 Phase A) is optional. If pkg-config finds
# avahi-client at build time we compile the real publisher; otherwise
# `--announce` prints a "rebuild with libavahi" message and the data path
# still works exactly as before.
HAVE_AVAHI := $(shell pkg-config --exists avahi-client && echo 1)
ifeq ($(HAVE_AVAHI),1)
CFLAGS  += -DAOETHER_HAVE_AVAHI $(shell pkg-config --cflags avahi-client)
LDLIBS  += $(shell pkg-config --libs avahi-client)
endif

# AVDECC entity (M7 Phase B) is also optional. When
# `../avdecc/build/libaoether_avdecc.a` exists we link it and define the
# flag so common/avdecc.c forwards to the C++ implementation; otherwise
# `--avdecc` prints a helpful error and the data path still works.
AVDECC_LIB := ../avdecc/build/libaoether_avdecc.a
HAVE_AVDECC := $(shell test -f $(AVDECC_LIB) && echo 1)
ifeq ($(HAVE_AVDECC),1)
CFLAGS  += -DAOETHER_HAVE_AVDECC
LDLIBS  += $(AVDECC_LIB) -lpcap -lstdc++ -lpthread
# Linker needs a C++ driver when pulling in C++ object code; switch to
# $(CXX) only when AVDECC is linked in.
LINK    := $(CXX)
else
LINK    := $(CC)
endif

.PHONY: all clean
all: $(BUILD)/receiver

$(BUILD)/receiver: $(SRCS) ../common/packet.h ../common/avtp.h ../common/rtp.h ../common/sdp.h ../common/sap.h ../common/mdns.h ../common/avdecc.h | $(BUILD)
	$(LINK) $(CFLAGS) $(INCLUDES) $(SRCS) -o $@ $(LDLIBS)

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
