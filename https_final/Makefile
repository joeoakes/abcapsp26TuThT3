PKG_CONFIG_PATH = /home/team3tt/local/lib/pkgconfig
export PKG_CONFIG_PATH

CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=c11
TARGET = maze_https_final
SRC = maze_https_mongo.c

PKGS = libmicrohttpd gnutls libbson-1.0 libmongoc-1.0
PKG_CFLAGS = $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --cflags $(PKGS))
PKG_LIBS = $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_PATH) pkg-config --libs $(PKGS))

INCLUDES = -I/home/team3tt/local/include
LIBPATH = -L/home/team3tt/local/lib
LIBS = -lhiredis
RPATH = -Wl,-rpath,/home/team3tt/local/lib

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) $(INCLUDES) $< -o $@ $(PKG_LIBS) $(LIBPATH) $(LIBS) $(RPATH)

clean:
	rm -f $(TARGET)
