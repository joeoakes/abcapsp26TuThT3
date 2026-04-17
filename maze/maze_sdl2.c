// maze_sdl2.c
// Simple SDL2 maze: generate (DFS backtracker), draw, move player to goal.
// Controls: Arrow keys, WASD, or analog stick. R = regenerate. Esc = quit.

#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <uuid/uuid.h>
#include <curl/curl.h>

#define MAZE_W 21   // number of cells horizontally
#define MAZE_H 15   // number of cells vertically
#define CELL   32   // pixels per cell
#define PAD    16   // window padding around maze

// Joystick dead zone threshold (0–32767 range)
#define JOYSTICK_DEADZONE 8000

// Default per-move telemetry URL
static const char* g_telemetry_url = "https://10.170.8.130:8445/move"; // "https://localhost:8445/move"

// mTLS client certificate paths (override via CLIENT_CERT, CLIENT_KEY, CA_CERT)
static const char* g_client_cert = "../https_final/certs/client.crt";
static const char* g_client_key  = "../https_final/certs/client.key";
static const char* g_ca_cert     = "../https_final/certs/ca.crt";

// Mini-Pupper mTLS cert paths (required when ROBOT_URL is set)
static const char* g_robot_client_cert = "../robot/certs/client.crt";
static const char* g_robot_client_key  = "../robot/certs/client.key";
static const char* g_robot_ca_cert     = "../robot/certs/ca.crt";

// Change env var to match the Mini-Pupper IP:
// ROBOT_URL="https://10.170.9.185:8445/robot" ./maze_sdl2
static const char* g_robot_url = "https://10.170.9.185:8445/robot";

// Session state for JSON telemetry
static char g_session_id[40];
static int  g_move_sequence = 0;

// Default mission summary endpoint
static const char* g_mission_url = "https://10.170.8.130:8445/mission"; // "https://localhost:8445/mission"

// Session timing and move counters for mission summary
static time_t g_session_start = 0;
static int g_moves_left = 0;
static int g_moves_right = 0;
static int g_moves_up = 0;
static int g_moves_down = 0;

// A* autonomous navigation state
static int  g_auto_nav_delay_ms = 5000; // ms between auto moves (default 5s)
static bool g_auto_nav = false;
typedef struct { int x, y; } Point;
static Point g_auto_path[MAZE_W * MAZE_H]; // Path from current pos to goal
static int g_auto_path_len = 0; // Total steps in path
static int g_auto_path_idx = 0; // Next step index
static Uint32 g_auto_last_move = 0; // Tick of last auto move

// Remote ML policy ("N" key) navigation state
// Policy server runs on the DGX Spark and returns a greedy action for each step.
static const char* g_policy_url = "https://10.170.8.109:8445/policy";
static const char* g_policy_client_cert = "../https_final/certs/client.crt";
static const char* g_policy_client_key  = "../https_final/certs/client.key";
static const char* g_policy_ca_cert     = "../https_final/certs/ca.crt";
static bool g_policy_verify_peer = true;
#define ML_NAV_MAX_STEPS 1000
static bool g_ml_nav = false;
static int g_ml_step_count = 0;
static Uint32 g_ml_last_move = 0;

// Visited-cell tracking (required for policy observation; mirrors MazeEnv._visit_count > 0)
static uint8_t g_visited[MAZE_H][MAZE_W];

// Wall bitmask for each cell (defined early so fetch_policy_action can read g[y][x].walls).
enum { WALL_N = 1, WALL_E = 2, WALL_S = 4, WALL_W = 8 };

typedef struct {
  uint8_t walls;
  bool visited;
} Cell;

static Cell g[MAZE_H][MAZE_W];

typedef struct {
  char* items[256];
  int head;
  int tail;
  int count;
  bool running;
  SDL_mutex* mutex;
  SDL_cond* cond;
  SDL_Thread* thread;
} TelemetryQueue;

static TelemetryQueue g_telemetry_queue;
static TelemetryQueue g_robot_queue;
static TelemetryQueue g_mission_queue;

// Discard curl response body
static size_t discard_response(void* ptr, size_t size, size_t nmemb, void* userdata) {
  (void)ptr; (void)userdata;
  return size * nmemb;
}

// POST JSON string to the telemetry server
static void post_json_to_server(const char* json) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    fprintf(stderr, "curl_easy_init failed\n");
    return;
  }

  struct curl_slist* headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, g_telemetry_url);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L); // 2 second timeout
  //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
  // mTLS: present client cert, verify server cert via CA
  curl_easy_setopt(curl, CURLOPT_SSLCERT,        g_client_cert);
  curl_easy_setopt(curl, CURLOPT_SSLKEY,         g_client_key);
  curl_easy_setopt(curl, CURLOPT_CAINFO,         g_ca_cert);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  
  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "curl POST failed: %s\n", curl_easy_strerror(res));
  } else {
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200)
      fprintf(stderr, "telemetry HTTP %ld\n", http_code);
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
}

// POST JSON string to the mission summary server
static void post_json_to_mission(const char* json) {
  if (!g_mission_url) return;

  CURL* curl = curl_easy_init();
  if (!curl) {
    fprintf(stderr, "curl_easy_init failed\n");
    return;
  }

  struct curl_slist* headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, g_mission_url);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
  // mTLS: present client cert, verify server cert via CA
  curl_easy_setopt(curl, CURLOPT_SSLCERT,        g_client_cert);
  curl_easy_setopt(curl, CURLOPT_SSLKEY,         g_client_key);
  curl_easy_setopt(curl, CURLOPT_CAINFO,         g_ca_cert);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "mission POST failed: %s\n", curl_easy_strerror(res));
  } else {
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code != 200)
      fprintf(stderr, "mission HTTP %ld\n", http_code);
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
}

// POST a robot command JSON to the Mini-Pupper bridge
static void post_robot_command(const char* json) {
  if (!g_robot_url) return;

  if (!g_robot_client_cert || !g_robot_client_key || !g_robot_ca_cert) {
    fprintf(stderr, "robot mTLS certs not configured; set ROBOT_CLIENT_CERT/ROBOT_CLIENT_KEY/ROBOT_CA_CERT\n");
    return;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    fprintf(stderr, "robot curl_easy_init failed\n");
    return;
  }

  struct curl_slist* headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, g_robot_url);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 2L);
  // mTLS: present client cert and verify robot cert against trusted CA
  curl_easy_setopt(curl, CURLOPT_SSLCERT,        g_robot_client_cert);
  curl_easy_setopt(curl, CURLOPT_SSLKEY,         g_robot_client_key);
  curl_easy_setopt(curl, CURLOPT_CAINFO,         g_robot_ca_cert);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "robot POST failed: %s\n", curl_easy_strerror(res));
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
}

// Collect curl response body into a dynamic buffer (used for synchronous requests).
typedef struct {
  char*  buf;
  size_t size;
  size_t cap;
} ResponseBuf;

static size_t collect_response(void* ptr, size_t size, size_t nmemb, void* userdata) {
  ResponseBuf* resp = (ResponseBuf*)userdata;
  size_t incoming = size * nmemb;
  size_t needed = resp->size + incoming + 1;
  if (needed > resp->cap) {
    size_t new_cap = resp->cap ? resp->cap : 256;
    while (new_cap < needed) new_cap *= 2;
    char* new_buf = (char*)realloc(resp->buf, new_cap);
    if (!new_buf) return 0; // abort transfer
    resp->buf = new_buf;
    resp->cap = new_cap;
  }
  memcpy(resp->buf + resp->size, ptr, incoming);
  resp->size += incoming;
  resp->buf[resp->size] = '\0';
  return incoming;
}

// Synchronously POST the current maze state to the remote policy server and
// parse the returned action (0=N, 1=E, 2=S, 3=W). Returns true on success.
static bool fetch_policy_action(int px, int py, int* action_out) {
  if (!g_policy_url || !action_out) return false;

  // Body: {"width":W,"height":H,"agent":[x,y],"goal":[gx,gy],"walls":[...],"visited":[...]}
  // Worst case: ~80 + W*H*3 + W*H*2 + slack. For 21x15 that's ~1700 bytes.
  static char body[4096];
  int n = snprintf(body, sizeof(body),
    "{\"width\":%d,\"height\":%d,\"agent\":[%d,%d],\"goal\":[%d,%d],\"walls\":[",
    MAZE_W, MAZE_H, px, py, MAZE_W - 1, MAZE_H - 1);

  for (int y = 0; y < MAZE_H && n < (int)sizeof(body); y++) {
    for (int x = 0; x < MAZE_W && n < (int)sizeof(body); x++) {
      const char* sep = (x == 0 && y == 0) ? "" : ",";
      n += snprintf(body + n, sizeof(body) - n, "%s%u", sep, (unsigned)g[y][x].walls);
    }
  }
  n += snprintf(body + n, sizeof(body) - n, "],\"visited\":[");
  for (int y = 0; y < MAZE_H && n < (int)sizeof(body); y++) {
    for (int x = 0; x < MAZE_W && n < (int)sizeof(body); x++) {
      const char* sep = (x == 0 && y == 0) ? "" : ",";
      n += snprintf(body + n, sizeof(body) - n, "%s%d", sep, g_visited[y][x] ? 1 : 0);
    }
  }
  n += snprintf(body + n, sizeof(body) - n, "]}");

  if (n >= (int)sizeof(body)) {
    fprintf(stderr, "policy body overflow (n=%d)\n", n);
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl) return false;

  ResponseBuf resp = {0};
  struct curl_slist* headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, g_policy_url);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)n);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect_response);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);

  // mTLS infrastructure (peer verification controlled by g_policy_verify_peer).
  if (g_policy_client_cert) curl_easy_setopt(curl, CURLOPT_SSLCERT, g_policy_client_cert);
  if (g_policy_client_key)  curl_easy_setopt(curl, CURLOPT_SSLKEY,  g_policy_client_key);
  if (g_policy_ca_cert)     curl_easy_setopt(curl, CURLOPT_CAINFO,  g_policy_ca_cert);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, g_policy_verify_peer ? 1L : 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, g_policy_verify_peer ? 2L : 0L);

  CURLcode res = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  bool ok = false;
  if (res != CURLE_OK) {
    fprintf(stderr, "policy POST failed: %s\n", curl_easy_strerror(res));
  } else if (http_code != 200) {
    fprintf(stderr, "policy HTTP %ld\n", http_code);
  } else if (resp.buf) {
    const char* p = strstr(resp.buf, "\"action\"");
    if (p) {
      p = strchr(p, ':');
      if (p) {
        int a = atoi(p + 1);
        if (a >= 0 && a < 4) {
          *action_out = a;
          ok = true;
        }
      }
    }
    if (!ok) fprintf(stderr, "policy bad response body: %s\n", resp.buf);
  }
  free(resp.buf);
  return ok;
}

// Generic queue worker — calls post_fn for each dequeued JSON string
typedef void (*post_fn_t)(const char*);

typedef struct {
  TelemetryQueue* queue;
  post_fn_t       post_fn;
} WorkerCtx;

static int queue_worker(void* userdata) {
  WorkerCtx* ctx = (WorkerCtx*)userdata;
  TelemetryQueue* queue = ctx->queue;
  post_fn_t post_fn = ctx->post_fn;

  while (true) {
    SDL_LockMutex(queue->mutex);
    while (queue->count == 0 && queue->running) {
      SDL_CondWait(queue->cond, queue->mutex);
    }

    if (queue->count == 0 && !queue->running) {
      SDL_UnlockMutex(queue->mutex);
      break;
    }

    char* json = queue->items[queue->head];
    queue->items[queue->head] = NULL;
    queue->head = (queue->head + 1) % (int)(sizeof(queue->items) / sizeof(queue->items[0]));
    queue->count--;
    bool still_running = queue->running;
    SDL_UnlockMutex(queue->mutex);

    if (json) {
      if (still_running)
        post_fn(json);
      free(json);
    }
  }

  free(ctx);
  return 0;
}

static bool init_queue(TelemetryQueue* queue, const char* name, post_fn_t post_fn) {
  memset(queue, 0, sizeof(*queue));
  queue->mutex = SDL_CreateMutex();
  queue->cond = SDL_CreateCond();
  if (!queue->mutex || !queue->cond) {
    if (queue->mutex) SDL_DestroyMutex(queue->mutex);
    if (queue->cond) SDL_DestroyCond(queue->cond);
    memset(queue, 0, sizeof(*queue));
    return false;
  }

  WorkerCtx* ctx = malloc(sizeof(WorkerCtx));
  if (!ctx) {
    SDL_DestroyCond(queue->cond);
    SDL_DestroyMutex(queue->mutex);
    memset(queue, 0, sizeof(*queue));
    return false;
  }
  ctx->queue = queue;
  ctx->post_fn = post_fn;

  queue->running = true;
  queue->thread = SDL_CreateThread(queue_worker, name, ctx);
  if (!queue->thread) {
    queue->running = false;
    free(ctx);
    SDL_DestroyCond(queue->cond);
    SDL_DestroyMutex(queue->mutex);
    memset(queue, 0, sizeof(*queue));
    return false;
  }

  return true;
}

static void shutdown_queue(TelemetryQueue* queue) {
  if (!queue->mutex) return; // never initialized

  SDL_LockMutex(queue->mutex);
  queue->running = false;
  SDL_CondSignal(queue->cond);
  SDL_UnlockMutex(queue->mutex);

  if (queue->thread) {
    SDL_WaitThread(queue->thread, NULL);
  }

  SDL_LockMutex(queue->mutex);
  while (queue->count > 0) {
    char* json = queue->items[queue->head];
    queue->items[queue->head] = NULL;
    queue->head = (queue->head + 1) % (int)(sizeof(queue->items) / sizeof(queue->items[0]));
    queue->count--;
    free(json);
  }
  SDL_UnlockMutex(queue->mutex);

  SDL_DestroyCond(queue->cond);
  SDL_DestroyMutex(queue->mutex);
  memset(queue, 0, sizeof(*queue));
}

static void enqueue_json(TelemetryQueue* queue, const char* label, const char* json) {
  char* copy = strdup(json);
  if (!copy) {
    fprintf(stderr, "%s enqueue failed: out of memory\n", label);
    return;
  }

  SDL_LockMutex(queue->mutex);
  if (queue->count == (int)(sizeof(queue->items) / sizeof(queue->items[0]))) {
    SDL_UnlockMutex(queue->mutex);
    fprintf(stderr, "%s queue full, dropping event\n", label);
    free(copy);
    return;
  }

  queue->items[queue->tail] = copy;
  queue->tail = (queue->tail + 1) % (int)(sizeof(queue->items) / sizeof(queue->items[0]));
  queue->count++;
  SDL_CondSignal(queue->cond);
  SDL_UnlockMutex(queue->mutex);
}

static void telemetry_enqueue_json(const char* json) {
  enqueue_json(&g_telemetry_queue, "telemetry", json);
}

typedef enum {
  ROBOT_HEADING_NORTH = 0,
  ROBOT_HEADING_EAST  = 1,
  ROBOT_HEADING_SOUTH = 2,
  ROBOT_HEADING_WEST  = 3,
} RobotHeading;

static void robot_enqueue_action(const char* action) {
  if (!g_robot_url) return;

  char json[128];
  snprintf(json, sizeof(json), "{\"action\":\"%s\"}", action);
  enqueue_json(&g_robot_queue, "robot", json);
}

static int robot_heading_from_move(int dx, int dy) {
  if (dx == 0 && dy == -1) return ROBOT_HEADING_NORTH;
  if (dx == 1 && dy == 0) return ROBOT_HEADING_EAST;
  if (dx == 0 && dy == 1) return ROBOT_HEADING_SOUTH;
  if (dx == -1 && dy == 0) return ROBOT_HEADING_WEST;
  return -1;
}

// Map a maze movement (dx,dy) to a robot action and enqueue it
static void robot_send_move(int dx, int dy, int* robot_heading) {
  if (!g_robot_url || !robot_heading) return;

  int desired_heading = robot_heading_from_move(dx, dy);
  if (desired_heading < 0) return;

  int turn_delta = (desired_heading - *robot_heading + 4) % 4;
  if (turn_delta == 1) {
    robot_enqueue_action("turn_right");
  } else if (turn_delta == 2) {
    robot_enqueue_action("turn_right");
    robot_enqueue_action("turn_right");
  } else if (turn_delta == 3) {
    robot_enqueue_action("turn_left");
  }

  robot_enqueue_action("forward");
  *robot_heading = desired_heading;
}

static inline bool in_bounds(int x, int y) {
  return (x >= 0 && x < MAZE_W && y >= 0 && y < MAZE_H);
}

// Remove wall between (x,y) and (nx,ny)
static void knock_down(int x, int y, int nx, int ny) {
  if (nx == x && ny == y - 1) { // N
    g[y][x].walls &= ~WALL_N;
    g[ny][nx].walls &= ~WALL_S;
  } else if (nx == x + 1 && ny == y) { // E
    g[y][x].walls &= ~WALL_E;
    g[ny][nx].walls &= ~WALL_W;
  } else if (nx == x && ny == y + 1) { // S
    g[y][x].walls &= ~WALL_S;
    g[ny][nx].walls &= ~WALL_N;
  } else if (nx == x - 1 && ny == y) { // W
    g[y][x].walls &= ~WALL_W;
    g[ny][nx].walls &= ~WALL_E;
  }
}

static void maze_init(void) {
  for (int y = 0; y < MAZE_H; y++) {
    for (int x = 0; x < MAZE_W; x++) {
      g[y][x].walls = WALL_N | WALL_E | WALL_S | WALL_W;
      g[y][x].visited = false;
    }
  }
}

// Iterative DFS "recursive backtracker"
static void maze_generate(int sx, int sy) {
  typedef struct { int x, y; } P;
  P stack[MAZE_W * MAZE_H];
  int top = 0;

  g[sy][sx].visited = true;
  stack[top++] = (P){sx, sy};

  while (top > 0) {
    P cur = stack[top - 1];
    int x = cur.x, y = cur.y;

    // Collect unvisited neighbors
    P neigh[4];
    int ncount = 0;

    const int dx[4] = { 0, 1, 0, -1 };
    const int dy[4] = { -1, 0, 1, 0 };

    for (int i = 0; i < 4; i++) {
      int nx = x + dx[i], ny = y + dy[i];
      if (in_bounds(nx, ny) && !g[ny][nx].visited) {
        neigh[ncount++] = (P){nx, ny};
      }
    }

    if (ncount == 0) {
      // backtrack
      top--;
      continue;
    }

    // choose random neighbor
    int pick = rand() % ncount;
    int nx = neigh[pick].x, ny = neigh[pick].y;

    // carve passage
    knock_down(x, y, nx, ny);
    g[ny][nx].visited = true;
    stack[top++] = (P){nx, ny};
  }

  // Clear visited flags so we can reuse for other logic later if needed
  for (int y = 0; y < MAZE_H; y++)
    for (int x = 0; x < MAZE_W; x++)
      g[y][x].visited = false;
}

// Draw maze walls as lines
static void draw_maze(SDL_Renderer* r) {
  // Background
  SDL_SetRenderDrawColor(r, 15, 15, 18, 255);
  SDL_RenderClear(r);

  // Maze lines
  SDL_SetRenderDrawColor(r, 230, 230, 230, 255);

  int ox = PAD;
  int oy = PAD;

  for (int y = 0; y < MAZE_H; y++) {
    for (int x = 0; x < MAZE_W; x++) {
      int x0 = ox + x * CELL;
      int y0 = oy + y * CELL;
      int x1 = x0 + CELL;
      int y1 = y0 + CELL;

      uint8_t w = g[y][x].walls;

      if (w & WALL_N) SDL_RenderDrawLine(r, x0, y0, x1, y0);
      if (w & WALL_E) SDL_RenderDrawLine(r, x1, y0, x1, y1);
      if (w & WALL_S) SDL_RenderDrawLine(r, x0, y1, x1, y1);
      if (w & WALL_W) SDL_RenderDrawLine(r, x0, y0, x0, y1);
    }
  }
}

// A* pathfinding: find shortest path from (sx,sy) to (gx,gy)
// Returns path length stored in g_auto_path[] (excluding start, including goal)
static int astar_solve(int sx, int sy, int gx, int gy) {
  typedef struct { int x, y, f, g_cost; } Node;
  static Node open[MAZE_W * MAZE_H];
  static int came_from[MAZE_H][MAZE_W]; // Encodes parent: y*MAZE_W+x, or -1
  static int cost[MAZE_H][MAZE_W];
  static bool closed[MAZE_H][MAZE_W];
  int open_count = 0;

  for (int y = 0; y < MAZE_H; y++)
    for (int x = 0; x < MAZE_W; x++) {
      came_from[y][x] = -1;
      cost[y][x] = 999999;
      closed[y][x] = false;
    }

  cost[sy][sx] = 0;
  int h = abs(gx - sx) + abs(gy - sy);
  open[open_count++] = (Node){sx, sy, h, 0};

  const int dx[4] = {0, 1, 0, -1};
  const int dy[4] = {-1, 0, 1, 0};
  const uint8_t wall_mask[4] = {WALL_N, WALL_E, WALL_S, WALL_W};

  while (open_count > 0) {
    // Find node with lowest f in open set
    int best = 0;
    for (int i = 1; i < open_count; i++)
      if (open[i].f < open[best].f) best = i;

    Node cur = open[best];
    open[best] = open[--open_count];

    if (cur.x == gx && cur.y == gy) break;
    if (closed[cur.y][cur.x]) continue;
    closed[cur.y][cur.x] = true;

    for (int d = 0; d < 4; d++) {
      if (g[cur.y][cur.x].walls & wall_mask[d]) continue;
      int nx = cur.x + dx[d], ny = cur.y + dy[d];
      if (!in_bounds(nx, ny) || closed[ny][nx]) continue;

      int new_g = cur.g_cost + 1;
      if (new_g < cost[ny][nx]) {
        cost[ny][nx] = new_g;
        came_from[ny][nx] = cur.y * MAZE_W + cur.x;
        int new_h = abs(gx - nx) + abs(gy - ny);
        open[open_count++] = (Node){nx, ny, new_g + new_h, new_g};
      }
    }
  }

  // Reconstruct path
  if (came_from[gy][gx] == -1 && !(sx == gx && sy == gy)) return 0;

  int path_len = 0;
  int cx = gx, cy = gy;
  while (!(cx == sx && cy == sy)) {
    g_auto_path[path_len++] = (Point){cx, cy};
    int prev = came_from[cy][cx];
    cx = prev % MAZE_W;
    cy = prev / MAZE_W;
  }
  // Reverse the path so index 0 is the first step
  for (int i = 0; i < path_len / 2; i++) {
    Point tmp = g_auto_path[i];
    g_auto_path[i] = g_auto_path[path_len - 1 - i];
    g_auto_path[path_len - 1 - i] = tmp;
  }
  return path_len;
}

// Player / goal rendering
static void draw_player_goal(SDL_Renderer* r, int px, int py, bool auto_mode, bool ml_mode) {
  int ox = PAD;
  int oy = PAD;

  // Goal cell highlight
  SDL_Rect goal = {
    ox + (MAZE_W - 1) * CELL + 6,
    oy + (MAZE_H - 1) * CELL + 6,
    CELL - 12,
    CELL - 12
  };
  SDL_SetRenderDrawColor(r, 40, 160, 70, 255);
  SDL_RenderFillRect(r, &goal);

  // Player
  SDL_Rect p = {
    ox + px * CELL + 8,
    oy + py * CELL + 8,
    CELL - 16,
    CELL - 16
  };

  // Color per mode: normal=yellow, A*=blue, ML=magenta
  if (ml_mode)
    SDL_SetRenderDrawColor(r, 200, 60, 220, 255);
  else if (auto_mode)
    SDL_SetRenderDrawColor(r, 50, 100, 230, 255);
  else
    SDL_SetRenderDrawColor(r, 230, 200, 40, 255);
  SDL_RenderFillRect(r, &p);
}

// Attempt to move player; returns true if moved
static bool try_move(int* px, int* py, int dx, int dy) {
  int x = *px, y = *py;
  int nx = x + dx, ny = y + dy;
  if (!in_bounds(nx, ny)) return false;

  uint8_t w = g[y][x].walls;

  // Blocked by wall?
  if (dx == 0 && dy == -1 && (w & WALL_N)) return false;
  if (dx == 1 && dy == 0  && (w & WALL_E)) return false;
  if (dx == 0 && dy == 1  && (w & WALL_S)) return false;
  if (dx == -1 && dy == 0 && (w & WALL_W)) return false;

  *px = nx;
  *py = ny;
  g_visited[ny][nx] = 1;
  return true;
}

// Forward declarations for session helpers used by regenerate
static void generate_session_id(void);
static void reset_session_stats(void);

static void regenerate(int* px, int* py, SDL_Window* win) {
  maze_init();
  maze_generate(0, 0);
  *px = 0; *py = 0;
  memset(g_visited, 0, sizeof(g_visited));
  g_visited[0][0] = 1;
  generate_session_id();
  reset_session_stats();
  SDL_SetWindowTitle(win, "SDL2 Maze - Reach the green goal (R to regenerate)");
}

static void generate_session_id(void) {
  uuid_t uuid;
  uuid_generate(uuid);
  uuid_unparse_lower(uuid, g_session_id);
}

// Get current timestamp in ISO 8601 format (UTC)
static void get_iso8601_timestamp(char* buf, size_t size) {
  time_t now = time(NULL);
  struct tm* gm = gmtime(&now);
  strftime(buf, size, "%Y-%m-%dT%H:%M:%SZ", gm);
}

static void reset_session_stats(void) {
  g_move_sequence = 0;
  g_moves_left = 0;
  g_moves_right = 0;
  g_moves_up = 0;
  g_moves_down = 0;
  g_session_start = time(NULL);
}

static void update_move_counters(int dx, int dy) {
  if (dx == -1) g_moves_left++;
  else if (dx == 1) g_moves_right++;
  else if (dy == -1) g_moves_up++;
  else if (dy == 1) g_moves_down++;
}

static void post_mission_summary(const char* result, const char* abort_reason) {
  if (!g_mission_url) return;

  time_t now = time(NULL);
  int64_t start_ts = (int64_t)g_session_start;
  int64_t end_ts = (int64_t)now;
  int64_t duration = end_ts - start_ts;
  if (duration < 0) duration = 0;

  int total = g_moves_left + g_moves_right + g_moves_up + g_moves_down;
  double distance = (double)total;

  char json[1024];
  snprintf(json, sizeof(json),
    "{"
    "\"mission_id\":\"%s\","
    "\"robot_id\":\"maze_player\","
    "\"mission_type\":\"maze\","
    "\"start_time\":%lld,"
    "\"end_time\":%lld,"
    "\"moves_left_turn\":%d,"
    "\"moves_right_turn\":%d,"
    "\"moves_straight\":%d,"
    "\"moves_reverse\":%d,"
    "\"moves_total\":%d,"
    "\"distance_traveled\":%.6f,"
    "\"duration_seconds\":%lld,"
    "\"mission_result\":\"%s\","
    "\"abort_reason\":\"%s\""
    "}",
    g_session_id,
    (long long)start_ts,
    (long long)end_ts,
    g_moves_left,
    g_moves_right,
    g_moves_up,
    g_moves_down,
    total,
    distance,
    (long long)duration,
    result,
    abort_reason ? abort_reason : "");

  printf("[mission summary] %s\n", json);
  fflush(stdout);

  enqueue_json(&g_mission_queue, "mission", json);
}

static void output_json_telemetry(const char* device, int px, int py, bool goal_reached) {
  char timestamp[32];
  get_iso8601_timestamp(timestamp, sizeof(timestamp));

  char json[1024];
  snprintf(json, sizeof(json),
    "{\n"
    "  \"session_id\": \"%s\",\n"
    "  \"event_type\": \"player_move\",\n"
    "  \"input\": {\n"
    "    \"device\": \"%s\",\n"
    "    \"move_sequence\": %d\n"
    "  },\n"
    "  \"player\": {\n"
    "    \"position\": { \"x\": %d, \"y\": %d }\n"
    "  },\n"
    "  \"goal_reached\": %s,\n"
    "  \"timestamp\": \"%s\"\n"
    "}\n",
    g_session_id, device, g_move_sequence, px, py,
    goal_reached ? "true" : "false", timestamp);

  printf("%s", json);
  fflush(stdout);

  // Enqueue for worker thread
  telemetry_enqueue_json(json);
}

// Send a move to the robot bridge — call after a successful try_move
static void maybe_send_robot(int dx, int dy, int* robot_heading) {
  robot_send_move(dx, dy, robot_heading);
}

// Handle joystick/controller axis input + emit telemetry on move
// Returns true after goal reached
static bool handle_joystick_axis(int axis, Sint16 value, int* px, int* py, bool* joy_moved_x, bool* joy_moved_y, bool* won, int* robot_heading, SDL_Window* win) {
  bool moved = false;
  int mdx = 0, mdy = 0;

  // X-axis (axis 0 for joystick, SDL_CONTROLLER_AXIS_LEFTX for controller)
  if (axis == 0) {
    if (value < -JOYSTICK_DEADZONE && !*joy_moved_x) {
      mdx = -1; mdy = 0;
      moved = try_move(px, py, mdx, mdy);
      *joy_moved_x = true;
    } else if (value > JOYSTICK_DEADZONE && !*joy_moved_x) {
      mdx = 1; mdy = 0;
      moved = try_move(px, py, mdx, mdy);
      *joy_moved_x = true;
    } else if (value > -JOYSTICK_DEADZONE && value < JOYSTICK_DEADZONE) {
      *joy_moved_x = false;
    }
  }

  // Y-axis (axis 1 for joystick, SDL_CONTROLLER_AXIS_LEFTY for controller)
  if (axis == 1) {
    if (value < -JOYSTICK_DEADZONE && !*joy_moved_y) {
      mdx = 0; mdy = -1;
      moved = try_move(px, py, mdx, mdy);
      *joy_moved_y = true;
    } else if (value > JOYSTICK_DEADZONE && !*joy_moved_y) {
      mdx = 0; mdy = 1;
      moved = try_move(px, py, mdx, mdy);
      *joy_moved_y = true;
    } else if (value > -JOYSTICK_DEADZONE && value < JOYSTICK_DEADZONE) {
      *joy_moved_y = false;
    }
  }

  if (moved) {
    update_move_counters(mdx, mdy);
    g_move_sequence++;
    bool goal_reached = (*px == MAZE_W - 1 && *py == MAZE_H - 1);
    output_json_telemetry("joystick", *px, *py, goal_reached);
    maybe_send_robot(mdx, mdy, robot_heading);

    if (goal_reached) {
      *won = true;
      post_mission_summary("success", "");
      SDL_SetWindowTitle(win, "You win! Press R to regenerate, Esc to quit");
    }
  }

  return moved;
}

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  srand((unsigned)time(NULL));

  // Initialize libcurl globally
  curl_global_init(CURL_GLOBAL_DEFAULT);

  // Check for telemetry URL override
  const char* env_url = getenv("TELEMETRY_URL");
  if (env_url && strlen(env_url) > 0) {
    g_telemetry_url = env_url;
  }
  printf("Telemetry URL: %s\n", g_telemetry_url);

  // Mission summary URL
  const char* env_mission = getenv("MISSION_URL");
  if (env_mission && strlen(env_mission) > 0) {
    g_mission_url = env_mission;
  }
  printf("Mission URL:   %s\n", g_mission_url ? g_mission_url : "(disabled)");

  // Robot bridge URL (optional)
  const char* env_robot = getenv("ROBOT_URL");
  if (env_robot && strlen(env_robot) > 0) {
    g_robot_url = env_robot;
  }
  printf("Robot URL:     %s\n", g_robot_url ? g_robot_url : "(disabled)");

  // mTLS cert path overrides
  const char* env_cc = getenv("CLIENT_CERT");
  if (env_cc && strlen(env_cc) > 0) g_client_cert = env_cc;
  const char* env_ck = getenv("CLIENT_KEY");
  if (env_ck && strlen(env_ck) > 0) g_client_key  = env_ck;
  const char* env_ca = getenv("CA_CERT");
  if (env_ca && strlen(env_ca) > 0) g_ca_cert     = env_ca;
  printf("mTLS client cert: %s\n", g_client_cert);
  printf("mTLS client key:  %s\n", g_client_key);
  printf("mTLS CA cert:     %s\n", g_ca_cert);

  // Mini-Pupper mTLS certs (required when ROBOT_URL is set)
  const char* env_rcc = getenv("ROBOT_CLIENT_CERT");
  if (env_rcc && strlen(env_rcc) > 0) g_robot_client_cert = env_rcc;
  const char* env_rck = getenv("ROBOT_CLIENT_KEY");
  if (env_rck && strlen(env_rck) > 0) g_robot_client_key = env_rck;
  const char* env_rca = getenv("ROBOT_CA_CERT");
  if (env_rca && strlen(env_rca) > 0) g_robot_ca_cert = env_rca;

  printf("Robot mTLS client cert: %s\n", g_robot_client_cert ? g_robot_client_cert : "(unset)");
  printf("Robot mTLS client key:  %s\n", g_robot_client_key  ? g_robot_client_key  : "(unset)");
  printf("Robot mTLS CA cert:     %s\n", g_robot_ca_cert     ? g_robot_ca_cert     : "(unset)");

  // Auto-nav delay (ms between A* moves, default 3000)
  const char* env_delay = getenv("AUTO_NAV_DELAY");
  if (env_delay && strlen(env_delay) > 0) {
    int d = atoi(env_delay);
    if (d > 0) g_auto_nav_delay_ms = d;
  }
  printf("Auto-nav delay:  %d ms\n", g_auto_nav_delay_ms);

  // Remote ML-policy server (press N in the maze window to invoke)
  const char* env_policy = getenv("POLICY_URL");
  if (env_policy && strlen(env_policy) > 0) g_policy_url = env_policy;

  const char* env_pcc = getenv("POLICY_CLIENT_CERT");
  if (env_pcc && strlen(env_pcc) > 0) g_policy_client_cert = env_pcc;
  const char* env_pck = getenv("POLICY_CLIENT_KEY");
  if (env_pck && strlen(env_pck) > 0) g_policy_client_key = env_pck;
  const char* env_pca = getenv("POLICY_CA_CERT");
  if (env_pca && strlen(env_pca) > 0) g_policy_ca_cert = env_pca;

  const char* env_pvp = getenv("POLICY_VERIFY_PEER");
  if (env_pvp && strlen(env_pvp) > 0) {
    char c = env_pvp[0];
    if (c == '1' || c == 't' || c == 'T' || c == 'y' || c == 'Y') {
      g_policy_verify_peer = true;
    } else if (c == '0' || c == 'f' || c == 'F' || c == 'n' || c == 'N') {
      g_policy_verify_peer = false;
    }
  }

  printf("Policy URL:      %s\n", g_policy_url ? g_policy_url : "(disabled)");
  printf("Policy client cert: %s\n", g_policy_client_cert ? g_policy_client_cert : "(unset)");
  printf("Policy client key:  %s\n", g_policy_client_key  ? g_policy_client_key  : "(unset)");
  printf("Policy CA cert:     %s\n", g_policy_ca_cert     ? g_policy_ca_cert     : "(unset)");
  printf("Policy verify peer: %s\n", g_policy_verify_peer ? "YES" : "NO");

  if (g_robot_url && (!g_robot_client_cert || !g_robot_client_key || !g_robot_ca_cert)) {
    fprintf(stderr, "ROBOT_URL is set but ROBOT_CLIENT_CERT/ROBOT_CLIENT_KEY/ROBOT_CA_CERT are required; robot link disabled\n");
    g_robot_url = NULL;
  }

  if (g_policy_url && g_policy_verify_peer &&
      (!g_policy_client_cert || !g_policy_client_key || !g_policy_ca_cert)) {
    fprintf(stderr, "POLICY_VERIFY_PEER is on but POLICY_CLIENT_CERT/POLICY_CLIENT_KEY/POLICY_CA_CERT are required; policy link disabled\n");
    g_policy_url = NULL;
  }

  // Generate session ID for telemetry
  generate_session_id();
  reset_session_stats();

  // For testing w/ PS5 controller
  //SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");

  // Initialize both VIDEO and JOYSTICK + GAMECONTROLLER subsystems
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  if (!init_queue(&g_telemetry_queue, "telemetry_worker", post_json_to_server)) {
    fprintf(stderr, "Failed to start telemetry worker\n");
    SDL_Quit();
    curl_global_cleanup();
    return 1;
  }

  if (g_mission_url) {
    if (!init_queue(&g_mission_queue, "mission_worker", post_json_to_mission)) {
      fprintf(stderr, "Failed to start mission worker (continuing without mission summaries)\n");
      g_mission_url = NULL;
    }
  }

  if (g_robot_url) {
    if (!init_queue(&g_robot_queue, "robot_worker", post_robot_command)) {
      fprintf(stderr, "Failed to start robot worker (continuing without robot)\n");
      g_robot_url = NULL; // disable robot commands
    }
  }

  // Enable joystick + game controller events
  SDL_JoystickEventState(SDL_ENABLE);
  SDL_GameControllerEventState(SDL_ENABLE);

  // Try to load controller mappings from gamecontrollerdb.txt if present (not using this right now)
  int mappings_added = SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
  if (mappings_added > 0) {
    printf("Loaded %d controller mapping(s) from gamecontrollerdb.txt\n", mappings_added);
  }

  // Print detected joysticks for debugging
  printf("Detected %d joystick(s):\n", SDL_NumJoysticks());
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    const char* name = SDL_JoystickNameForIndex(i);
    bool is_gc = SDL_IsGameController(i);
    printf("  [%d] %s (GameController: %s)\n", i, name ? name : "Unknown", is_gc ? "Yes" : "No");
  }

  // Open game controller if available
  SDL_GameController* controller = NULL;
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      controller = SDL_GameControllerOpen(i);
      if (controller) {
        printf("Controller connected: %s\n", SDL_GameControllerName(controller));
        break;
      }
    }
  }

  // If no GameController recognized, fall back to raw joystick (will likely need this for Game HAT)
  SDL_Joystick* joystick = NULL;
  if (!controller && SDL_NumJoysticks() > 0) {
    joystick = SDL_JoystickOpen(0);
    if (joystick) {
      printf("Opened raw joystick: %s (axes: %d)\n",
             SDL_JoystickName(joystick), SDL_JoystickNumAxes(joystick));
    }
  }

  int win_w = PAD * 2 + MAZE_W * CELL;
  int win_h = PAD * 2 + MAZE_H * CELL;

  int window_x = SDL_WINDOWPOS_CENTERED;
  int window_y = SDL_WINDOWPOS_CENTERED;
  const char* env_display = getenv("MAZE_DISPLAY");
  if (env_display && strlen(env_display) > 0) {
    char* endptr = NULL;
    long display_index = strtol(env_display, &endptr, 10);
    if (endptr == env_display || *endptr != '\0') {
      fprintf(stderr, "Invalid MAZE_DISPLAY value '%s'; expected an integer display index\n", env_display);
    } else if (display_index < 0 || display_index >= SDL_GetNumVideoDisplays()) {
      fprintf(stderr, "MAZE_DISPLAY=%ld out of range; SDL reports %d display(s)\n", display_index, SDL_GetNumVideoDisplays());
    } else {
      window_x = SDL_WINDOWPOS_CENTERED_DISPLAY((int)display_index);
      window_y = SDL_WINDOWPOS_CENTERED_DISPLAY((int)display_index);
      printf("Using SDL display %ld for maze window\n", display_index);
    }
  }

  SDL_Window* win = SDL_CreateWindow(
    "SDL2 Maze - Reach the green goal (R to regenerate)",
    window_x, window_y,
    win_w, win_h,
    SDL_WINDOW_SHOWN
  );
  if (!win) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    if (controller) SDL_GameControllerClose(controller);
    SDL_Quit();
    return 1;
  }

  SDL_Renderer* r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!r) {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    if (controller) SDL_GameControllerClose(controller);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 1;
  }

  int px = 0, py = 0;
  regenerate(&px, &py, win);
  int robot_heading = ROBOT_HEADING_NORTH;

  bool running = true;
  bool won = false;

  // Joystick state tracking (for discrete-step movement)
  bool joy_moved_x = false;
  bool joy_moved_y = false;

  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) running = false;

      if (e.type == SDL_KEYDOWN) {
        SDL_Keycode k = e.key.keysym.sym;

        if (k == SDLK_ESCAPE) running = false;

        if (k == SDLK_r) {
          if (!won) post_mission_summary("aborted", "player regenerated");
          g_auto_nav = false;
          g_ml_nav = false;
          regenerate(&px, &py, win);
          robot_heading = ROBOT_HEADING_NORTH;
          won = false;
        }

        if (k == SDLK_m && !won) {
          if (!g_auto_nav) {
            g_ml_nav = false; // cannot run both modes at once
            // Compute A* path from current position to goal
            g_auto_path_len = astar_solve(px, py, MAZE_W - 1, MAZE_H - 1);
            g_auto_path_idx = 0;
            g_auto_last_move = SDL_GetTicks();
            g_auto_nav = (g_auto_path_len > 0);
            if (g_auto_nav)
              printf("[A*] auto-nav ON  (%d steps to goal)\n", g_auto_path_len);
            else
              printf("[A*] no path found\n");
          } else {
            g_auto_nav = false;
            printf("[A*] auto-nav OFF\n");
          }
        }

        // Remote ML-policy auto-navigation
        if (k == SDLK_n && !won) {
          if (!g_ml_nav) {
            // 4/17/26 note: The trained checkpoint is fragile when agent starts mid-maze, so reset to default start
            if (px != 0 || py != 0) {
              px = 0;
              py = 0;
              robot_heading = ROBOT_HEADING_NORTH;
              printf("[ML] agent not at start; reset to (0,0)\n");
            }
            memset(g_visited, 0, sizeof(g_visited));
            g_visited[0][0] = 1;
            g_auto_nav = false; // cannot run both modes at once
            g_ml_step_count = 0;
            g_ml_last_move = SDL_GetTicks() - (Uint32)g_auto_nav_delay_ms; // act immediately
            g_ml_nav = true;
            printf("[ML] auto-nav ON  (policy_url=%s, step_limit=%d, delay=%dms)\n",
                   g_policy_url, ML_NAV_MAX_STEPS, g_auto_nav_delay_ms);
          } else {
            g_ml_nav = false;
            printf("[ML] auto-nav OFF\n");
          }
        }

        if (!won) {
          bool moved = false;
          int mdx = 0, mdy = 0;
          if (k == SDLK_UP || k == SDLK_w)          { mdx = 0; mdy = -1; }
          else if (k == SDLK_RIGHT || k == SDLK_d)  { mdx = 1; mdy = 0; }
          else if (k == SDLK_DOWN || k == SDLK_s)   { mdx = 0; mdy = 1; }
          else if (k == SDLK_LEFT || k == SDLK_a)   { mdx = -1; mdy = 0; }
          if (mdx != 0 || mdy != 0) moved = try_move(&px, &py, mdx, mdy);

          if (moved) {
            update_move_counters(mdx, mdy);
            g_move_sequence++;
            bool goal_reached = (px == MAZE_W - 1 && py == MAZE_H - 1);
            output_json_telemetry("keyboard", px, py, goal_reached);
            maybe_send_robot(mdx, mdy, &robot_heading);

            if (goal_reached) {
              won = true;
              post_mission_summary("success", "");
              SDL_SetWindowTitle(win, "You win! Press R to regenerate, Esc to quit");
            }
          }
        }
      }

      // Handle game controller axis motion (recognized controllers)
      if (e.type == SDL_CONTROLLERAXISMOTION && !won) {
        int axis = (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) ? 0 : (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) ? 1 : -1;
        if (axis >= 0) {
          handle_joystick_axis(axis, e.caxis.value, &px, &py, &joy_moved_x, &joy_moved_y, &won, &robot_heading, win);
        }
      }

      // Fallback (raw joystick axis motion)
      if (e.type == SDL_JOYAXISMOTION && !won && joystick) {
        handle_joystick_axis(e.jaxis.axis, e.jaxis.value, &px, &py, &joy_moved_x, &joy_moved_y, &won, &robot_heading, win);
      }
    }

    // Remote ML-policy auto-navigation: advance one step on the same timer
    // cadence that A* uses. If the policy can't reach the goal in
    // ML_NAV_MAX_STEPS attempts, give up and turn auto-nav off.
    if (g_ml_nav && !won) {
      Uint32 now = SDL_GetTicks();
      if (now - g_ml_last_move >= (Uint32)g_auto_nav_delay_ms) {
        g_ml_last_move = now;
        int action = -1;
        if (!fetch_policy_action(px, py, &action)) {
          fprintf(stderr, "[ML] policy request failed; auto-nav OFF\n");
          g_ml_nav = false;
        } else {
          // action: 0=N, 1=E, 2=S, 3=W
          const int action_dx[4] = { 0, 1, 0, -1 };
          const int action_dy[4] = {-1, 0, 1,  0 };
          int mdx = action_dx[action];
          int mdy = action_dy[action];
          bool moved = try_move(&px, &py, mdx, mdy);
          g_ml_step_count++;

          if (moved) {
            update_move_counters(mdx, mdy);
            g_move_sequence++;
            bool goal_reached = (px == MAZE_W - 1 && py == MAZE_H - 1);
            output_json_telemetry("ml_policy", px, py, goal_reached);
            maybe_send_robot(mdx, mdy, &robot_heading);

            if (goal_reached) {
              won = true;
              g_ml_nav = false;
              post_mission_summary("success", "");
              SDL_SetWindowTitle(win, "You win! Press R to regenerate, Esc to quit");
              printf("[ML] goal reached in %d policy steps\n", g_ml_step_count);
            }
          } else {
            // Policy tried to walk into a wall; still counts toward the cap.
            printf("[ML] step %d: blocked action=%d (dx=%d,dy=%d)\n", g_ml_step_count, action, mdx, mdy);
          }

          if (g_ml_nav && g_ml_step_count >= ML_NAV_MAX_STEPS) {
            printf("[ML] step cap reached (%d); auto-nav OFF\n", ML_NAV_MAX_STEPS);
            g_ml_nav = false;
          }
        }
      }
    }

    // A* auto-navigation: advance one step on timer
    if (g_auto_nav && !won && g_auto_path_idx < g_auto_path_len) {
      Uint32 now = SDL_GetTicks();
      if (now - g_auto_last_move >= (Uint32)g_auto_nav_delay_ms) {
        Point next = g_auto_path[g_auto_path_idx];
        int mdx = next.x - px;
        int mdy = next.y - py;
        bool moved = try_move(&px, &py, mdx, mdy);
        if (moved) {
          update_move_counters(mdx, mdy);
          g_move_sequence++;
          bool goal_reached = (px == MAZE_W - 1 && py == MAZE_H - 1);
          output_json_telemetry("a_star", px, py, goal_reached);
          maybe_send_robot(mdx, mdy, &robot_heading);
          g_auto_path_idx++;
          g_auto_last_move = now;

          if (goal_reached) {
            won = true;
            g_auto_nav = false;
            post_mission_summary("success", "");
            SDL_SetWindowTitle(win, "You win! Press R to regenerate, Esc to quit");
          }
        } else {
          // Path blocked (this case should never happen...)
          g_auto_nav = false;
        }
      }
    }

    draw_maze(r);
    draw_player_goal(r, px, py, g_auto_nav, g_ml_nav);

    SDL_RenderPresent(r);
  }

  // Send final summary if user quit without winning
  if (!won) post_mission_summary("aborted", "user exited");

  if (controller) SDL_GameControllerClose(controller);
  if (joystick) SDL_JoystickClose(joystick);
  SDL_DestroyRenderer(r);
  SDL_DestroyWindow(win);
  if (g_robot_url) shutdown_queue(&g_robot_queue);
  if (g_mission_url) shutdown_queue(&g_mission_queue);
  shutdown_queue(&g_telemetry_queue);
  SDL_Quit();
  curl_global_cleanup();
  return 0;
}
