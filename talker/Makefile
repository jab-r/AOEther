CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -D_GNU_SOURCE
LDLIBS  ?= -lm

BUILD     := build
INCLUDES  := -Isrc -I../common
SRCS      := \
    src/talker.c \
    src/audio_source_test.c \
    src/audio_source_wav.c \
    ../common/packet.c

.PHONY: all clean
all: $(BUILD)/talker

$(BUILD)/talker: $(SRCS) src/audio_source.h ../common/packet.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) -o $@ $(LDLIBS)

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
