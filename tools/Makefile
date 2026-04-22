CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -D_GNU_SOURCE

BUILD   := build
INCLUDES := -I../common

# mDNS-SD via Avahi is optional. If pkg-config finds avahi-client, compile
# the real helper; otherwise the binary prints a "rebuild with libavahi"
# message at runtime.
HAVE_AVAHI := $(shell pkg-config --exists avahi-client && echo 1)
ifeq ($(HAVE_AVAHI),1)
CFLAGS  += -DAOETHER_HAVE_AVAHI $(shell pkg-config --cflags avahi-client)
LDLIBS  += $(shell pkg-config --libs avahi-client)
endif

SRCS := src/aoether_browse.c

.PHONY: all clean
all: $(BUILD)/aoether-browse

$(BUILD)/aoether-browse: $(SRCS) | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) -o $@ $(LDLIBS)

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
