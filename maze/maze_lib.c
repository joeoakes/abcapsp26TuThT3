// Build: gcc -shared -fPIC -O2 -o libmaze.so maze_lib.c

#include "maze_lib.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t walls;
} Cell;

struct MazeInstance {
    int w, h;
    Cell* grid; // row-major: grid[y * w + x]
};

static inline int in_bounds(const MazeInstance* m, int x, int y) {
    return x >= 0 && x < m->w && y >= 0 && y < m->h;
}

static inline Cell* cell_at(const MazeInstance* m, int x, int y) {
    return &m->grid[y * m->w + x];
}

// Remove wall between adjacent cells (x,y) and (nx,ny)
static void knock_down(MazeInstance* m, int x, int y, int nx, int ny) {
    if (nx == x && ny == y - 1) {
        cell_at(m, x, y)->walls  &= ~WALL_N;
        cell_at(m, nx, ny)->walls &= ~WALL_S;
    } else if (nx == x + 1 && ny == y) {
        cell_at(m, x, y)->walls  &= ~WALL_E;
        cell_at(m, nx, ny)->walls &= ~WALL_W;
    } else if (nx == x && ny == y + 1) {
        cell_at(m, x, y)->walls  &= ~WALL_S;
        cell_at(m, nx, ny)->walls &= ~WALL_N;
    } else if (nx == x - 1 && ny == y) {
        cell_at(m, x, y)->walls  &= ~WALL_W;
        cell_at(m, nx, ny)->walls &= ~WALL_E;
    }
}

// Simple xorshift32 RNG (deterministic + seed-controlled)
static uint32_t xorshift32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

// Iterative DFS backtracker maze generation
static void generate(MazeInstance* m, uint32_t seed) {
    uint32_t rng = seed ? seed : 1;
    int total = m->w * m->h;

    // Initialize all walls
    for (int i = 0; i < total; i++)
        m->grid[i].walls = WALL_N | WALL_E | WALL_S | WALL_W;

    uint8_t* visited = calloc(total, 1);
    if (!visited) return;

    typedef struct { int x, y; } P;
    P* stack = malloc(total * sizeof(P));
    if (!stack) { free(visited); return; }

    int top = 0;
    int sx = 0, sy = 0;
    visited[sy * m->w + sx] = 1;
    stack[top++] = (P){sx, sy};

    const int dx[4] = {0, 1, 0, -1};
    const int dy[4] = {-1, 0, 1, 0};

    while (top > 0) {
        P cur = stack[top - 1];
        int x = cur.x, y = cur.y;

        // Collect unvisited neighbors
        P neigh[4];
        int nc = 0;
        for (int d = 0; d < 4; d++) {
            int nx = x + dx[d], ny = y + dy[d];
            if (in_bounds(m, nx, ny) && !visited[ny * m->w + nx])
                neigh[nc++] = (P){nx, ny};
        }

        if (nc == 0) {
            top--;
            continue;
        }

        int pick = xorshift32(&rng) % nc;
        int nx = neigh[pick].x, ny = neigh[pick].y;
        knock_down(m, x, y, nx, ny);
        visited[ny * m->w + nx] = 1;
        stack[top++] = (P){nx, ny};
    }

    free(stack);
    free(visited);
}

// Public API

MazeInstance* maze_create(int width, int height, uint32_t seed) {
    if (width < 2 || height < 2 || width > MAZE_MAX_W || height > MAZE_MAX_H)
        return NULL;

    MazeInstance* m = malloc(sizeof(MazeInstance));
    if (!m) return NULL;

    m->w = width;
    m->h = height;
    m->grid = calloc(width * height, sizeof(Cell));
    if (!m->grid) { free(m); return NULL; }

    generate(m, seed);
    return m;
}

void maze_free(MazeInstance* m) {
    if (!m) return;
    free(m->grid);
    free(m);
}

int maze_width(const MazeInstance* m)  { return m ? m->w : 0; }
int maze_height(const MazeInstance* m) { return m ? m->h : 0; }

uint8_t maze_get_walls(const MazeInstance* m, int x, int y) {
    if (!m || !in_bounds(m, x, y)) return 0xF;
    return cell_at(m, x, y)->walls;
}

void maze_get_wall_grid(const MazeInstance* m, uint8_t* out) {
    if (!m || !out) return;
    for (int i = 0; i < m->w * m->h; i++)
        out[i] = m->grid[i].walls;
}

int maze_can_move(const MazeInstance* m, int x, int y, int dx, int dy) {
    if (!m || !in_bounds(m, x, y)) return 0;
    int nx = x + dx, ny = y + dy;
    if (!in_bounds(m, nx, ny)) return 0;

    uint8_t w = cell_at(m, x, y)->walls;
    if (dx == 0 && dy == -1 && (w & WALL_N)) return 0;
    if (dx == 1 && dy == 0  && (w & WALL_E)) return 0;
    if (dx == 0 && dy == 1  && (w & WALL_S)) return 0;
    if (dx == -1 && dy == 0 && (w & WALL_W)) return 0;
    return 1;
}

int maze_compute_distance_map(const MazeInstance* m, int gx, int gy, int32_t* dist_out) {
    if (!m || !dist_out || !in_bounds(m, gx, gy)) return -1;

    int total = m->w * m->h;
    for (int i = 0; i < total; i++)
        dist_out[i] = -1;

    // BFS from goal
    typedef struct { int x, y; } P;
    P* queue = malloc(total * sizeof(P));
    if (!queue) return -1;

    int head = 0, tail = 0;
    dist_out[gy * m->w + gx] = 0;
    queue[tail++] = (P){gx, gy};

    const int dx[4] = {0, 1, 0, -1};
    const int dy[4] = {-1, 0, 1, 0};
    const uint8_t wall_mask[4] = {WALL_N, WALL_E, WALL_S, WALL_W};
    // Opposite wall to check from neighbor side
    const uint8_t opp_mask[4] = {WALL_S, WALL_W, WALL_N, WALL_E};

    while (head < tail) {
        P cur = queue[head++];
        int cd = dist_out[cur.y * m->w + cur.x];

        for (int d = 0; d < 4; d++) {
            // Check wall from current cell in direction d
            if (cell_at(m, cur.x, cur.y)->walls & wall_mask[d]) continue;

            int nx = cur.x + dx[d], ny = cur.y + dy[d];
            if (!in_bounds(m, nx, ny)) continue;
            if (dist_out[ny * m->w + nx] >= 0) continue; // Already visited

            dist_out[ny * m->w + nx] = cd + 1;
            queue[tail++] = (P){nx, ny};
        }
    }

    free(queue);

    return dist_out[0]; // Return distance from (0, 0)
}

int maze_astar(const MazeInstance* m, int sx, int sy, int gx, int gy, int* path_x, int* path_y) {
    if (!m || !path_x || !path_y) return 0;
    if (!in_bounds(m, sx, sy) || !in_bounds(m, gx, gy)) return 0;
    if (sx == gx && sy == gy) return 0;

    int total = m->w * m->h;

    typedef struct { int x, y, f, g_cost; } Node;
    Node* open = malloc(total * sizeof(Node));
    int* came_from = malloc(total * sizeof(int));
    int* cost = malloc(total * sizeof(int));
    uint8_t* closed = calloc(total, 1);
    if (!open || !came_from || !cost || !closed) {
        free(open); free(came_from); free(cost); free(closed);
        return 0;
    }

    for (int i = 0; i < total; i++) {
        came_from[i] = -1;
        cost[i] = 999999;
    }

    cost[sy * m->w + sx] = 0;
    int h = abs(gx - sx) + abs(gy - sy);
    int open_count = 0;
    open[open_count++] = (Node){sx, sy, h, 0};

    const int dx[4] = {0, 1, 0, -1};
    const int dy[4] = {-1, 0, 1, 0};
    const uint8_t wall_mask[4] = {WALL_N, WALL_E, WALL_S, WALL_W};

    while (open_count > 0) {
        int best = 0;
        for (int i = 1; i < open_count; i++)
            if (open[i].f < open[best].f) best = i;

        Node cur = open[best];
        open[best] = open[--open_count];

        if (cur.x == gx && cur.y == gy) break;
        int ci = cur.y * m->w + cur.x;
        if (closed[ci]) continue;
        closed[ci] = 1;

        for (int d = 0; d < 4; d++) {
            if (cell_at(m, cur.x, cur.y)->walls & wall_mask[d]) continue;
            int nx = cur.x + dx[d], ny = cur.y + dy[d];
            if (!in_bounds(m, nx, ny)) continue;
            int ni = ny * m->w + nx;
            if (closed[ni]) continue;

            int new_g = cur.g_cost + 1;
            if (new_g < cost[ni]) {
                cost[ni] = new_g;
                came_from[ni] = ci;
                int new_h = abs(gx - nx) + abs(gy - ny);
                open[open_count++] = (Node){nx, ny, new_g + new_h, new_g};
            }
        }
    }

    // Reconstruct path
    int gi = gy * m->w + gx;
    if (came_from[gi] == -1) {
        free(open); free(came_from); free(cost); free(closed);
        return 0;
    }

    int path_len = 0;
    int ci = gi;
    while (ci != sy * m->w + sx) {
        path_x[path_len] = ci % m->w;
        path_y[path_len] = ci / m->w;
        path_len++;
        ci = came_from[ci];
    }

    // Reverse
    for (int i = 0; i < path_len / 2; i++) {
        int tx = path_x[i], ty = path_y[i];
        path_x[i] = path_x[path_len - 1 - i];
        path_y[i] = path_y[path_len - 1 - i];
        path_x[path_len - 1 - i] = tx;
        path_y[path_len - 1 - i] = ty;
    }

    free(open);
    free(came_from);
    free(cost);
    free(closed);
    return path_len;
}
