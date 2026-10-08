# Pingu Player - Parcial 2 Sistemas Operativos
CC      ?= gcc
OPT     ?= -O2 -g
SAN     ?=
BUILD   ?= build
CFLAGS_ALL  = $(OPT) $(SAN) -std=gnu11 -Wall -Wextra -Wshadow -pthread $(CFLAGS)
LDFLAGS_ALL = $(SAN) -pthread $(LDFLAGS)
LDLIBS_ALL  = -lm $(LDLIBS)

LIB_SRC = ringbuf playlist source spawn audio_out player
LIB_OBJ = $(LIB_SRC:%=$(BUILD)/%.o)

.PHONY: all samples check test tsan asan clean run

all: $(BUILD)/pingu $(BUILD)/gentone

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS_ALL) -c $< -o $@

$(BUILD)/pingu: $(LIB_OBJ) $(BUILD)/ui.o $(BUILD)/main.o
	$(CC) $(LDFLAGS_ALL) $^ -o $@ $(LDLIBS_ALL)

$(BUILD)/gentone: tools/gentone.c | $(BUILD)
	$(CC) $(OPT) -std=gnu11 -Wall -Wextra $< -o $@ -lm

$(BUILD)/test_ringbuf: tests/test_ringbuf.c $(BUILD)/ringbuf.o | $(BUILD)
	$(CC) $(CFLAGS_ALL) -Isrc $^ -o $@ $(LDFLAGS_ALL)

$(BUILD)/test_playlist: tests/test_playlist.c $(BUILD)/playlist.o | $(BUILD)
	$(CC) $(CFLAGS_ALL) -Isrc $^ -o $@ $(LDFLAGS_ALL)

# 5 pistas de ejemplo (arpegios) para probar sin musica propia
samples: $(BUILD)/gentone
	mkdir -p samples
	$(BUILD)/gentone samples/01_do.wav   261.63 12
	$(BUILD)/gentone samples/02_re.wav   293.66 12
	$(BUILD)/gentone samples/03_mi.wav   329.63 12
	$(BUILD)/gentone samples/04_sol.wav  392.00 12
	$(BUILD)/gentone samples/05_la.wav   440.00 12
	@echo "Listo: samples/*.wav"

check: $(BUILD)/pingu $(BUILD)/gentone $(BUILD)/test_ringbuf $(BUILD)/test_playlist
	$(BUILD)/test_ringbuf
	$(BUILD)/test_playlist
	BUILD=$(BUILD) bash tests/smoke.sh

test: check

# Pruebas con ThreadSanitizer (detecta carreras) y AddressSanitizer (memoria)
tsan:
	$(MAKE) BUILD=build-tsan SAN=-fsanitize=thread OPT="-O1 -g" check

asan:
	$(MAKE) BUILD=build-asan SAN="-fsanitize=address,undefined" OPT="-O1 -g" check

run: all samples
	$(BUILD)/pingu --shuffle --play samples/

clean:
	rm -rf build build-tsan build-asan samples
