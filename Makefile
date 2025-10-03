CC      := gcc
CFLAGS  := -std=c11 -O2 -Wall -Wextra -Werror -pedantic
BUILD   := build
TARGET  := $(BUILD)/connect4

SRCS    := $(wildcard src/*.c)
OBJS    := $(patsubst src/%.c,$(BUILD)/%.o,$(SRCS))

.PHONY: all run clean dirs

all: dirs $(TARGET)

dirs:
	@mkdir -p $(BUILD)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	$(TARGET)

clean:
	rm -rf $(BUILD)
