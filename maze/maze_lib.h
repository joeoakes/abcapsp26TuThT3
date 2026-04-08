#ifndef MAZE_LIB_H
#define MAZE_LIB_H

#include <stdint.h>

// Wall bitmask constants (same as maze_sdl2.c)
#define WALL_N 1
#define WALL_E 2
#define WALL_S 4
#define WALL_W 8

// Maximum supported maze dimensions
#define MAZE_MAX_W 64
#define MAZE_MAX_H 64

// Opaque maze handle
typedef struct MazeInstance MazeInstance;

// Create a new maze with the given dimensions and RNG seed
// Returns NULL on failure. Caller must free with maze_free()
MazeInstance* maze_create(int width, int height, uint32_t seed);

// Free a maze instance
void maze_free(MazeInstance* m);

// Get maze dimensions
int maze_width(const MazeInstance* m);
int maze_height(const MazeInstance* m);

// Get wall bitmask for cell (x, y). Returns 0xF if out of bounds
uint8_t maze_get_walls(const MazeInstance* m, int x, int y);

// Copy the full wall grid into a caller-provided buffer (row-major, H*W bytes)
void maze_get_wall_grid(const MazeInstance* m, uint8_t* out);

// Check if movement from (x,y) in direction (dx,dy) is legal (no wall)
// Returns 1 if passable, 0 if blocked or out of bounds
int maze_can_move(const MazeInstance* m, int x, int y, int dx, int dy);

// Compute BFS distance map from (gx, gy) to every reachable cell
// Writes H*W int32 values into dist_out (row-major). Unreachable cells get -1
// Returns the distance from (sx, sy) to (gx, gy), or -1 if unreachable
int maze_compute_distance_map(const MazeInstance* m, int gx, int gy, int32_t* dist_out);

// A* solve: find shortest path from (sx,sy) to (gx,gy)
// Writes path coordinates into path_x[], path_y[] (excluding start, including goal)
// Returns path length, or 0 if no path found
// path_x, path_y must have room for at least width*height entries
int maze_astar(const MazeInstance* m, int sx, int sy, int gx, int gy, int* path_x, int* path_y);

#endif // MAZE_LIB_H
