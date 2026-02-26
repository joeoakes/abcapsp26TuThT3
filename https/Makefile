CC = gcc
TARGET = maze_https_mongo
SRC = maze_https_mongo.c

CFLAGS = -O2 -Wall -Wextra -std=c11 $(shell pkg-config --cflags libmicrohttpd libmongoc-1.0 gnutls)
LDFLAGS = $(shell pkg-config --libs libmicrohttpd libmongoc-1.0 gnutls)

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean
