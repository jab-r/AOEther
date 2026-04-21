CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=c11 -D_GNU_SOURCE
LDLIBS  ?= -lm -lasound

BUILD     := build
INCLUDES  := -Isrc -I../common
SRCS      := \
    src/talker.c \
    src/audio_source_test.c \
    src/audio_source_wav.c \
    src/audio_source_alsa.c \
    ../common/packet.c \
    ../common/avtp.c

.PHONY: all clean
all: $(BUILD)/talker

$(BUILD)/talker: $(SRCS) src/audio_source.h ../common/packet.h ../common/avtp.h | $(BUILD)
	$(CC) $(CFLAGS) $(INCLUDES) $(SRCS) -o $@ $(LDLIBS)

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
