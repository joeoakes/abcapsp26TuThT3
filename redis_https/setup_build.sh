#!/usr/bin/env bash
set -euo pipefail

echo "[1/1] Building maze_https_redis..."
gcc -O2 -Wall -Wextra -std=c11 maze_https_mongo.c -o maze_https_redis $(pkg-config --cflags --libs libmicrohttpd gnutls libbson-1.0 hiredis)

echo "Build complete. Run: ./maze_https_redis"
