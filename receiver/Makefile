CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -D_GNU_SOURCE
LDLIBS  ?= -lasound

BUILD     := build
INCLUDES  := -Isrc -I../common
SRCS      := \
    src/receiver.c \
    ../common/packet.c \
    ../common/avtp.c \
    ../common/mdns.c

# mDNS-SD advertisement (M7 Phase A) is optional. If pkg-config finds
# avahi-client at build time we compile the real publisher; otherwise
# `--announce` prints a "rebuild with libavahi" message and the data path
# still works exactly as before.
HAVE_AVAHI := $(shell pkg-config --exists avahi-client && echo 1)
ifeq ($(HAVE_AVAHI),1)
CFLAGS  += -DAOETHER_HAVE_AVAHI $(shell pkg-config --cflags avahi-client)
LDLIBS  += $(shell pkg-config --libs avahi-client)
endif

.PHONY: all clean
all: $(BUILD)/receiver

$(BUILD)/receiver: $(SRCS) ../common/packet.h ../common/avtp.h ../common/mdns.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) -o $@ $(LDLIBS)

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
