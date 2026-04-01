CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11
TARGET = maze_https_local
SRC = maze_https_mongo.c

PKGS = libmicrohttpd gnutls libbson-1.0 libmongoc-1.0
PKG_CFLAGS = $(shell pkg-config --cflags $(PKGS))
PKG_LIBS = $(shell pkg-config --libs $(PKGS))

LIBS = -lhiredis

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) $< -o $@ $(PKG_LIBS) $(LIBS)

clean:
	rm -f $(TARGET)
