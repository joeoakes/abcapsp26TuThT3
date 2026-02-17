#!/usr/bin/env bash
set -euo pipefail

echo "[1/3] Updating package lists..."
sudo apt-get update

echo "[2/3] Installing build dependencies..."
sudo apt-get install -y build-essential pkg-config libmicrohttpd-dev libgnutls28-dev libbson-dev libhiredis-dev

echo "[3/3] Building maze_https_redis..."
gcc -O2 -Wall -Wextra -std=c11 maze_https_mongo.c -o maze_https_redis $(pkg-config --cflags --libs libmicrohttpd gnutls libbson-1.0 hiredis)

echo "Build complete. Run: ./maze_https_redis"
