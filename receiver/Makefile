CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -D_GNU_SOURCE
LDLIBS  ?= -lasound

BUILD     := build
INCLUDES  := -Isrc -I../common
SRCS      := \
    src/receiver.c \
    ../common/packet.c

.PHONY: all clean
all: $(BUILD)/receiver

$(BUILD)/receiver: $(SRCS) ../common/packet.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) -o $@ $(LDLIBS)

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
