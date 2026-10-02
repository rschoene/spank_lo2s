# Makefile for the SPANK plugin spank_lo2s.c

CC      ?= gcc
CFLAGS  ?= -Wall
CFLAGS  += -shared -fPIC
CFLAGS  += -I/usr/include/slurm -I.

TARGET   = spank_lo2s.so
SRC      = spank_lo2s.c

# Optional: path to the lo2s binary, e.g.
#   make LO2S_BINARY_PATH=/opt/lo2s/bin/lo2s
# If not set, the plugin falls back to its built-in default (/usr/bin/lo2s).
ifneq ($(LO2S_BINARY_PATH),)
CFLAGS += -DLO2S_BINARY_PATH="$(LO2S_BINARY_PATH)"
else
$(info NOTE: LO2S_BINARY_PATH is not set; using the built-in default /usr/bin/lo2s)
endif

.PHONY: all release debug clean

# Default build: optimized (-O2)
all: CFLAGS += -O2
all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $(SRC)

# Optimized release build (no debug symbols, stripped)
release: CFLAGS += -O3 -DNDEBUG
release: clean $(TARGET)
	strip $(TARGET)

# Build with debug logging enabled (defines LO2S_DEBUG)
debug: CFLAGS += -O0 -g -DLO2S_DEBUG
debug: clean $(TARGET)

# Build with debug logging and GDB crash backtrace capture (defines LO2S_DEBUG and GDB)
# When GDB is defined, the plugin runs lo2s under GDB in batch mode so that
# a crash backtrace is written to the log.
debug-gdb: CFLAGS += -O0 -g -DLO2S_DEBUG -DGDB
debug-gdb: clean $(TARGET)

clean:
	rm -f $(TARGET)
