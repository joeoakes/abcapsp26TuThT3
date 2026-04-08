CC = gcc
TARGET = maze_sdl2
SRC = maze_sdl2.c

CFLAGS = $(shell sdl2-config --cflags)
LDFLAGS = $(shell sdl2-config --libs) -luuid -lcurl

# Shared library for Python bridge
LIB_SRC = maze_lib.c
LIB_SO  = libmaze.so

all: $(TARGET) $(LIB_SO)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

$(LIB_SO): $(LIB_SRC) maze_lib.h
	$(CC) -shared -fPIC -O2 $(LIB_SRC) -o $(LIB_SO)

clean:
	rm -f $(TARGET) $(LIB_SO)

.PHONY: all clean