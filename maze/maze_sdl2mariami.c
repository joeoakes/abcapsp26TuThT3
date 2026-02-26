// maze_sdl2.c
// Simple SDL2 maze: generate (DFS backtracker), draw, move player to goal.
// Controls: Arrow keys, WASD, or analog stick. R = regenerate. Esc = quit.
//
// Telemetry: on session end (win or Esc), POSTs a mission summary JSON to
// the Redis-backed HTTPS server at TELEMETRY_URL (default: https://localhost:8445/mission).
// Per-move events are also still POSTed to /move if you want both.

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

// Joystick dead zone threshold (0-32767 range)
#define JOYSTICK_DEADZONE 8000

// Default HTTP server endpoint for telemetry (mission summary)
// Run with: TELEMETRY_URL="https://172.24.205.173:8445/mission" ./maze_sdl2
static const char* g_telemetry_url = "https://10.170.8.109:8445/mission";

// Per-move endpoint (optional second queue — set to NULL to disable)
// Set MOVE_URL env var to enable per-move telemetry alongside mission summary.
static const char* g_move_url = NULL;

// mTLS client certificate paths (override via CLIENT_CERT, CLIENT_KEY, CA_CERT)
static const char* g_client_cert = "../redis_https/certs/client.crt";
static const char* g_client_key  = "../redis_https/certs/client.key";
static const char* g_ca_cert     = "../redis_https/certs/ca.crt";

// Change env var to match the Mini-Pupper IP:
// ROBOT_URL="https://10.170.9.185:8445/robot" ./maze_sdl2
static const char* g_robot_url = NULL;

// Session state for JSON telemetry
static char   g_session_id[40];
static int    g_move_sequence = 0;
static time_t g_session_start = 0;

// Move counters for mission summary
static int g_moves_left  = 0;   // dx=-1  -> turn_left
static int g_moves_right = 0;   // dx=+1  -> turn_right
static int g_moves_up    = 0;   // dy=-1  -> straight/forward
static int g_moves_down  = 0;   // dy=+1  -> reverse/backward

// -----------------------------------------------------------------------
// Queue
// -----------------------------------------------------------------------

typedef struct {
  char* items[256];
  int head;
  int tail;
  int count;
  bool running;
  SDL_mutex* mutex;
  SDL_cond*  cond;
  SDL_Thread* thread;
} TelemetryQueue;

static TelemetryQueue g_telemetry_queue;  // mission summary queue
static TelemetryQueue g_move_queue;       // per-move queue (optional)
static TelemetryQueue g_robot_queue;

// Discard curl response body
static size_t discard_response(void* ptr, size_t size, size_t nmemb, void* userdata) {
  (void)ptr; (void)userdata;
  return size * nmemb;
}

// -----------------------------------------------------------------------
// Generic mTLS POST helper
// -----------------------------------------------------------------------

static void post_json_to_url(const char* url, const char* json) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    fprintf(stderr, "curl_easy_init failed\n");
    return;
  }

  struct curl_slist* headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL,           url);
  curl_easy_setopt(curl, CURLOPT_POST,          1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    json);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,       5L);
  // mTLS
  curl_easy_setopt(curl, CURLOPT_SSLCERT,        g_client_cert);
  curl_easy_setopt(curl, CURLOPT_SSLKEY,         g_client_key);
  curl_easy_setopt(curl, CURLOPT_CAINFO,         g_ca_cert);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 1L);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "curl POST to %s failed: %s\n", url, curl_easy_strerror(res));
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
}

// Wrappers bound to specific URLs (used as queue post_fn callbacks)
static void post_mission_json(const char* json) {
  post_json_to_url(g_telemetry_url, json);
}

static void post_move_json(const char* json) {
  if (g_move_url) post_json_to_url(g_move_url, json);
}

static void post_robot_command(const char* json) {
  if (!g_robot_url) return;

  CURL* curl = curl_easy_init();
  if (!curl) return;

  struct curl_slist* headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL,           g_robot_url);
  curl_easy_setopt(curl, CURLOPT_POST,          1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS,    json);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER,    headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT,       2L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "robot POST failed: %s\n", curl_easy_strerror(res));
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
}

// -----------------------------------------------------------------------
// Queue implementation
// -----------------------------------------------------------------------

typedef void (*post_fn_t)(const char*);

typedef struct {
  TelemetryQueue* queue;
  post_fn_t       post_fn;
} WorkerCtx;

static int queue_worker(void* userdata) {
  WorkerCtx* ctx       = (WorkerCtx*)userdata;
  TelemetryQueue* queue = ctx->queue;
  post_fn_t post_fn    = ctx->post_fn;

  while (true) {
    SDL_LockMutex(queue->mutex);
    while (queue->count == 0 && queue->running)
      SDL_CondWait(queue->cond, queue->mutex);

    if (queue->count == 0 && !queue->running) {
      SDL_UnlockMutex(queue->mutex);
      break;
    }

    char* json = queue->items[queue->head];
    queue->items[queue->head] = NULL;
    queue->head = (queue->head + 1) % (int)(sizeof(queue->items) / sizeof(queue->items[0]));
    queue->count--;
    SDL_UnlockMutex(queue->mutex);

    if (json) { post_fn(json); free(json); }
  }

  free(ctx);
  return 0;
}

static bool init_queue(TelemetryQueue* queue, const char* name, post_fn_t post_fn) {
  memset(queue, 0, sizeof(*queue));
  queue->mutex = SDL_CreateMutex();
  queue->cond  = SDL_CreateCond();
  if (!queue->mutex || !queue->cond) {
    if (queue->mutex) SDL_DestroyMutex(queue->mutex);
    if (queue->cond)  SDL_DestroyCond(queue->cond);
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
  ctx->queue   = queue;
  ctx->post_fn = post_fn;

  queue->running = true;
  queue->thread  = SDL_CreateThread(queue_worker, name, ctx);
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
  if (!queue->mutex) return;

  SDL_LockMutex(queue->mutex);
  queue->running = false;
  SDL_CondSignal(queue->cond);
  SDL_UnlockMutex(queue->mutex);

  if (queue->thread) SDL_WaitThread(queue->thread, NULL);

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
  if (!copy) { fprintf(stderr, "%s enqueue failed: out of memory\n", label); return; }

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

// -----------------------------------------------------------------------
// Robot
// -----------------------------------------------------------------------

static void robot_send_move(int dx, int dy) {
  if (!g_robot_url) return;

  const char* action = NULL;
  if      (dx == 0  && dy == -1) action = "forward";
  else if (dx == 0  && dy == 1)  action = "backward";
  else if (dx == -1 && dy == 0)  action = "turn_left";
  else if (dx == 1  && dy == 0)  action = "turn_right";
  else return;

  char json[128];
  snprintf(json, sizeof(json), "{\"action\":\"%s\"}", action);
  enqueue_json(&g_robot_queue, "robot", json);
}

// -----------------------------------------------------------------------
// Maze generation
// -----------------------------------------------------------------------

enum { WALL_N = 1, WALL_E = 2, WALL_S = 4, WALL_W = 8 };

typedef struct { uint8_t walls; bool visited; } Cell;
static Cell g[MAZE_H][MAZE_W];

static inline bool in_bounds(int x, int y) {
  return (x >= 0 && x < MAZE_W && y >= 0 && y < MAZE_H);
}

static void knock_down(int x, int y, int nx, int ny) {
  if      (nx == x && ny == y - 1) { g[y][x].walls &= ~WALL_N; g[ny][nx].walls &= ~WALL_S; }
  else if (nx == x + 1 && ny == y) { g[y][x].walls &= ~WALL_E; g[ny][nx].walls &= ~WALL_W; }
  else if (nx == x && ny == y + 1) { g[y][x].walls &= ~WALL_S; g[ny][nx].walls &= ~WALL_N; }
  else if (nx == x - 1 && ny == y) { g[y][x].walls &= ~WALL_W; g[ny][nx].walls &= ~WALL_E; }
}

static void maze_init(void) {
  for (int y = 0; y < MAZE_H; y++)
    for (int x = 0; x < MAZE_W; x++) {
      g[y][x].walls   = WALL_N | WALL_E | WALL_S | WALL_W;
      g[y][x].visited = false;
    }
}

static void maze_generate(int sx, int sy) {
  typedef struct { int x, y; } P;
  P stack[MAZE_W * MAZE_H];
  int top = 0;
  const int dx[4] = { 0, 1, 0, -1 };
  const int dy[4] = { -1, 0, 1, 0 };

  g[sy][sx].visited = true;
  stack[top++] = (P){sx, sy};

  while (top > 0) {
    P cur = stack[top - 1];
    int x = cur.x, y = cur.y;
    P neigh[4]; int ncount = 0;
    for (int i = 0; i < 4; i++) {
      int nx = x + dx[i], ny = y + dy[i];
      if (in_bounds(nx, ny) && !g[ny][nx].visited)
        neigh[ncount++] = (P){nx, ny};
    }
    if (ncount == 0) { top--; continue; }
    int pick = rand() % ncount;
    int nx = neigh[pick].x, ny = neigh[pick].y;
    knock_down(x, y, nx, ny);
    g[ny][nx].visited = true;
    stack[top++] = (P){nx, ny};
  }

  for (int y = 0; y < MAZE_H; y++)
    for (int x = 0; x < MAZE_W; x++)
      g[y][x].visited = false;
}

// -----------------------------------------------------------------------
// Rendering
// -----------------------------------------------------------------------

static void draw_maze(SDL_Renderer* r) {
  SDL_SetRenderDrawColor(r, 15, 15, 18, 255);
  SDL_RenderClear(r);
  SDL_SetRenderDrawColor(r, 230, 230, 230, 255);
  int ox = PAD, oy = PAD;
  for (int y = 0; y < MAZE_H; y++) {
    for (int x = 0; x < MAZE_W; x++) {
      int x0 = ox + x * CELL, y0 = oy + y * CELL;
      int x1 = x0 + CELL,     y1 = y0 + CELL;
      uint8_t w = g[y][x].walls;
      if (w & WALL_N) SDL_RenderDrawLine(r, x0, y0, x1, y0);
      if (w & WALL_E) SDL_RenderDrawLine(r, x1, y0, x1, y1);
      if (w & WALL_S) SDL_RenderDrawLine(r, x0, y1, x1, y1);
      if (w & WALL_W) SDL_RenderDrawLine(r, x0, y0, x0, y1);
    }
  }
}

static void draw_player_goal(SDL_Renderer* r, int px, int py) {
  int ox = PAD, oy = PAD;
  SDL_Rect goal = { ox + (MAZE_W-1)*CELL+6, oy + (MAZE_H-1)*CELL+6, CELL-12, CELL-12 };
  SDL_SetRenderDrawColor(r, 40, 160, 70, 255);
  SDL_RenderFillRect(r, &goal);
  SDL_Rect p = { ox + px*CELL+8, oy + py*CELL+8, CELL-16, CELL-16 };
  SDL_SetRenderDrawColor(r, 230, 200, 40, 255);
  SDL_RenderFillRect(r, &p);
}

// -----------------------------------------------------------------------
// Player movement
// -----------------------------------------------------------------------

static bool try_move(int* px, int* py, int dx, int dy) {
  int x = *px, y = *py, nx = x + dx, ny = y + dy;
  if (!in_bounds(nx, ny)) return false;
  uint8_t w = g[y][x].walls;
  if (dx == 0  && dy == -1 && (w & WALL_N)) return false;
  if (dx == 1  && dy == 0  && (w & WALL_E)) return false;
  if (dx == 0  && dy == 1  && (w & WALL_S)) return false;
  if (dx == -1 && dy == 0  && (w & WALL_W)) return false;
  *px = nx; *py = ny;
  return true;
}

// -----------------------------------------------------------------------
// Session helpers
// -----------------------------------------------------------------------

static void generate_session_id(void) {
  uuid_t uuid;
  uuid_generate(uuid);
  uuid_unparse_lower(uuid, g_session_id);
}

static void get_iso8601_timestamp(char* buf, size_t size) {
  time_t now = time(NULL);
  struct tm* gm = gmtime(&now);
  strftime(buf, size, "%Y-%m-%dT%H:%M:%SZ", gm);
}

// Reset all per-session counters (call on new game / regenerate)
static void reset_session_stats(void) {
  g_move_sequence = 0;
  g_moves_left    = 0;
  g_moves_right   = 0;
  g_moves_up      = 0;
  g_moves_down    = 0;
  g_session_start = time(NULL);
}

// -----------------------------------------------------------------------
// Telemetry: per-move event (optional, sent to g_move_url)
// -----------------------------------------------------------------------

static void emit_move_telemetry(const char* device, int px, int py, bool goal_reached) {
  char timestamp[32];
  get_iso8601_timestamp(timestamp, sizeof(timestamp));

  char json[1024];
  snprintf(json, sizeof(json),
    "{"
    "\"session_id\":\"%s\","
    "\"event_type\":\"player_move\","
    "\"input\":{\"device\":\"%s\",\"move_sequence\":%d},"
    "\"player\":{\"position\":{\"x\":%d,\"y\":%d}},"
    "\"goal_reached\":%s,"
    "\"timestamp\":\"%s\""
    "}",
    g_session_id, device, g_move_sequence, px, py,
    goal_reached ? "true" : "false", timestamp);

  printf("%s\n", json);
  fflush(stdout);

  // Optional per-move HTTP POST
  if (g_move_url) enqueue_json(&g_move_queue, "move", json);
}

// -----------------------------------------------------------------------
// Telemetry: mission summary (sent to Redis server at g_telemetry_url)
// -----------------------------------------------------------------------

static void post_mission_summary(const char* result, const char* abort_reason) {
  time_t now          = time(NULL);
  int64_t start_ts    = (int64_t)g_session_start;
  int64_t end_ts      = (int64_t)now;
  int64_t duration    = end_ts - start_ts;
  if (duration < 0) duration = 0;

  int total = g_moves_left + g_moves_right + g_moves_up + g_moves_down;
  // Distance: each cell = 1 unit, diagonal not possible, so total moves = distance
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

  enqueue_json(&g_telemetry_queue, "telemetry", json);
}

// -----------------------------------------------------------------------
// Regenerate maze + new session
// -----------------------------------------------------------------------

static void regenerate(int* px, int* py, SDL_Window* win) {
  maze_init();
  maze_generate(0, 0);
  *px = 0; *py = 0;
  generate_session_id();
  reset_session_stats();
  SDL_SetWindowTitle(win, "SDL2 Maze - Reach the green goal (R to regenerate)");
}

// -----------------------------------------------------------------------
// Move handling with counter updates
// -----------------------------------------------------------------------

static void update_move_counters(int dx, int dy) {
  if      (dx == -1) g_moves_left++;
  else if (dx == 1)  g_moves_right++;
  else if (dy == -1) g_moves_up++;
  else if (dy == 1)  g_moves_down++;
}

static bool handle_joystick_axis(int axis, Sint16 value,
                                  int* px, int* py,
                                  bool* joy_moved_x, bool* joy_moved_y,
                                  bool* won, SDL_Window* win)
{
  bool moved = false;
  int mdx = 0, mdy = 0;

  if (axis == 0) {
    if      (value < -JOYSTICK_DEADZONE && !*joy_moved_x) { mdx = -1; moved = try_move(px, py, mdx, mdy); *joy_moved_x = true; }
    else if (value >  JOYSTICK_DEADZONE && !*joy_moved_x) { mdx =  1; moved = try_move(px, py, mdx, mdy); *joy_moved_x = true; }
    else if (value > -JOYSTICK_DEADZONE && value < JOYSTICK_DEADZONE) { *joy_moved_x = false; }
  }
  if (axis == 1) {
    if      (value < -JOYSTICK_DEADZONE && !*joy_moved_y) { mdy = -1; moved = try_move(px, py, mdx, mdy); *joy_moved_y = true; }
    else if (value >  JOYSTICK_DEADZONE && !*joy_moved_y) { mdy =  1; moved = try_move(px, py, mdx, mdy); *joy_moved_y = true; }
    else if (value > -JOYSTICK_DEADZONE && value < JOYSTICK_DEADZONE) { *joy_moved_y = false; }
  }

  if (moved) {
    update_move_counters(mdx, mdy);
    g_move_sequence++;
    bool goal_reached = (*px == MAZE_W - 1 && *py == MAZE_H - 1);
    emit_move_telemetry("joystick", *px, *py, goal_reached);
    robot_send_move(mdx, mdy);

    if (goal_reached) {
      *won = true;
      post_mission_summary("success", "");
      SDL_SetWindowTitle(win, "You win! Press R to regenerate, Esc to quit");
    }
  }

  return moved;
}

// -----------------------------------------------------------------------
// main
// -----------------------------------------------------------------------

int main(int argc, char** argv) {
  (void)argc; (void)argv;
  srand((unsigned)time(NULL));

  curl_global_init(CURL_GLOBAL_DEFAULT);

  // Telemetry (mission summary) URL
  const char* env_url = getenv("TELEMETRY_URL");
  if (env_url && *env_url) g_telemetry_url = env_url;
  printf("Mission telemetry URL: %s\n", g_telemetry_url);

  // Optional per-move URL
  const char* env_move = getenv("MOVE_URL");
  if (env_move && *env_move) g_move_url = env_move;
  printf("Per-move URL:          %s\n", g_move_url ? g_move_url : "(disabled)");

  // Robot bridge URL
  const char* env_robot = getenv("ROBOT_URL");
  if (env_robot && *env_robot) g_robot_url = env_robot;
  printf("Robot URL:             %s\n", g_robot_url ? g_robot_url : "(disabled)");

  // mTLS cert overrides
  const char* env_cc = getenv("CLIENT_CERT"); if (env_cc && *env_cc) g_client_cert = env_cc;
  const char* env_ck = getenv("CLIENT_KEY");  if (env_ck && *env_ck) g_client_key  = env_ck;
  const char* env_ca = getenv("CA_CERT");     if (env_ca && *env_ca) g_ca_cert     = env_ca;
  printf("mTLS client cert: %s\n", g_client_cert);
  printf("mTLS client key:  %s\n", g_client_key);
  printf("mTLS CA cert:     %s\n", g_ca_cert);

  // Generate first session ID
  generate_session_id();
  reset_session_stats();

  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }

  // Mission summary queue (always required)
  if (!init_queue(&g_telemetry_queue, "telemetry_worker", post_mission_json)) {
    fprintf(stderr, "Failed to start telemetry worker\n");
    SDL_Quit(); curl_global_cleanup(); return 1;
  }

  // Optional per-move queue
  if (g_move_url) {
    if (!init_queue(&g_move_queue, "move_worker", post_move_json)) {
      fprintf(stderr, "Failed to start move worker (continuing without per-move telemetry)\n");
      g_move_url = NULL;
    }
  }

  // Robot queue
  if (g_robot_url) {
    if (!init_queue(&g_robot_queue, "robot_worker", post_robot_command)) {
      fprintf(stderr, "Failed to start robot worker (continuing without robot)\n");
      g_robot_url = NULL;
    }
  }

  SDL_JoystickEventState(SDL_ENABLE);
  SDL_GameControllerEventState(SDL_ENABLE);

  int mappings_added = SDL_GameControllerAddMappingsFromFile("gamecontrollerdb.txt");
  if (mappings_added > 0)
    printf("Loaded %d controller mapping(s)\n", mappings_added);

  printf("Detected %d joystick(s):\n", SDL_NumJoysticks());
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    const char* name = SDL_JoystickNameForIndex(i);
    printf("  [%d] %s (GameController: %s)\n", i,
           name ? name : "Unknown", SDL_IsGameController(i) ? "Yes" : "No");
  }

  SDL_GameController* controller = NULL;
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (SDL_IsGameController(i)) {
      controller = SDL_GameControllerOpen(i);
      if (controller) { printf("Controller: %s\n", SDL_GameControllerName(controller)); break; }
    }
  }

  SDL_Joystick* joystick = NULL;
  if (!controller && SDL_NumJoysticks() > 0) {
    joystick = SDL_JoystickOpen(0);
    if (joystick)
      printf("Raw joystick: %s (axes: %d)\n", SDL_JoystickName(joystick), SDL_JoystickNumAxes(joystick));
  }

  int win_w = PAD * 2 + MAZE_W * CELL;
  int win_h = PAD * 2 + MAZE_H * CELL;

  SDL_Window* win = SDL_CreateWindow(
    "SDL2 Maze - Reach the green goal (R to regenerate)",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_w, win_h, SDL_WINDOW_SHOWN);
  if (!win) {
    fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
    if (controller) SDL_GameControllerClose(controller);
    SDL_Quit(); return 1;
  }

  SDL_Renderer* r = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (!r) {
    fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
    if (controller) SDL_GameControllerClose(controller);
    SDL_DestroyWindow(win); SDL_Quit(); return 1;
  }

  int px = 0, py = 0;
  regenerate(&px, &py, win);

  bool running     = true;
  bool won         = false;
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
          // Send summary for the abandoned session before regenerating
          if (!won) post_mission_summary("aborted", "player regenerated");
          regenerate(&px, &py, win);
          won = false;
        }

        if (!won) {
          int mdx = 0, mdy = 0;
          if      (k == SDLK_UP    || k == SDLK_w) { mdy = -1; }
          else if (k == SDLK_RIGHT || k == SDLK_d) { mdx =  1; }
          else if (k == SDLK_DOWN  || k == SDLK_s) { mdy =  1; }
          else if (k == SDLK_LEFT  || k == SDLK_a) { mdx = -1; }

          if (mdx != 0 || mdy != 0) {
            bool moved = try_move(&px, &py, mdx, mdy);
            if (moved) {
              update_move_counters(mdx, mdy);
              g_move_sequence++;
              bool goal_reached = (px == MAZE_W - 1 && py == MAZE_H - 1);
              emit_move_telemetry("keyboard", px, py, goal_reached);
              robot_send_move(mdx, mdy);

              if (goal_reached) {
                won = true;
                post_mission_summary("success", "");
                SDL_SetWindowTitle(win, "You win! Press R to regenerate, Esc to quit");
              }
            }
          }
        }
      }

      // Game controller axis
      if (e.type == SDL_CONTROLLERAXISMOTION && !won) {
        int axis = -1;
        if      (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) axis = 0;
        else if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) axis = 1;
        if (axis >= 0)
          handle_joystick_axis(axis, e.caxis.value, &px, &py, &joy_moved_x, &joy_moved_y, &won, win);
      }

      // Raw joystick axis
      if (e.type == SDL_JOYAXISMOTION && !won && joystick)
        handle_joystick_axis(e.jaxis.axis, e.jaxis.value, &px, &py, &joy_moved_x, &joy_moved_y, &won, win);
    }

    draw_maze(r);
    draw_player_goal(r, px, py);
    SDL_RenderPresent(r);
  }

  // Send final summary if user quit without winning
  if (!won) post_mission_summary("aborted", "user exited");

  // Drain queues before shutting down SDL (the workers do the actual HTTP calls)
  if (g_robot_url) shutdown_queue(&g_robot_queue);
  if (g_move_url)  shutdown_queue(&g_move_queue);
  shutdown_queue(&g_telemetry_queue);

  if (controller) SDL_GameControllerClose(controller);
  if (joystick)   SDL_JoystickClose(joystick);
  SDL_DestroyRenderer(r);
  SDL_DestroyWindow(win);
  SDL_Quit();
  curl_global_cleanup();
  return 0;
}
