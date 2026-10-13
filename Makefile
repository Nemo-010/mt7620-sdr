CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -Werror -std=c11 -Iinclude
BUILD   := build

all: $(BUILD)/capture_test

$(BUILD)/capture_test: core/capture.c core/capture_test.c \
		include/mt7620_pbf.h core/capture.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ core/capture.c core/capture_test.c

$(BUILD):
	mkdir -p $(BUILD)

check: $(BUILD)/capture_test
	$(BUILD)/capture_test

clean:
	rm -rf $(BUILD)

.PHONY: all check clean
