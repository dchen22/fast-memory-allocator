CC = cc
CFLAGS = -std=c11 -Wall -Wextra -Werror -Isrc

BUILD = build

.PHONY: all test clean

all: $(BUILD)/tlsf

$(BUILD)/tlsf: $(BUILD)/main.o $(BUILD)/tlsf.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/test-malloc: $(BUILD)/test-malloc.o $(BUILD)/tlsf.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/test-realloc: $(BUILD)/test-realloc.o $(BUILD)/tlsf.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^

$(BUILD)/main.o: src/main.c src/tlsf.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/tlsf.o: src/tlsf.c src/tlsf.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/test-malloc.o: tests/test-malloc.c src/tlsf.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/test-realloc.o: tests/test-realloc.c src/tlsf.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	mkdir -p $(BUILD)

test: $(BUILD)/test-malloc $(BUILD)/test-realloc

clean:
	rm -rf $(BUILD)
