CC = gcc
CFLAGS = -std=c11 -O2 -Wall -Wextra -pedantic
BUILD = build

all: $(BUILD)/schedlab $(BUILD)/linux_launcher

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/schedlab: src/schedlab.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

$(BUILD)/linux_launcher: src/linux_launcher.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -rf $(BUILD) *.csv

.PHONY: all clean
