CC ?= cc
CFLAGS ?= -O3 -march=native -mtune=native -flto -fomit-frame-pointer -fno-plt -fno-semantic-interposition -falign-functions=64 -DNDEBUG
CFLAGS += -std=gnu17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -pthread
LDFLAGS ?= -flto -Wl,-O2 -Wl,--as-needed -pthread

.PHONY: all clean

all: tu104-bar1-overlay

tu104-bar1-overlay: tu104_bar1_overlay.c box_stream_protocol.h
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f tu104-bar1-overlay
