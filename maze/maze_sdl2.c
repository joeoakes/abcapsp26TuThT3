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

// Default HTTP server endpoint for telemetry
// Run with: `TELEMETRY_URL="http://172.24.205.173:8080/move" ./maze_sdl2`
// On WSL: `export MONGO_URI="mongodb://172.21.128.1:27017"`, then `./maze_http_mongo`
static const char* g_telemetry_url = "http://localhost:8443/move";

// Session state for JSON telemetry
static char g_session_id[40];
static int  g_move_sequence = 0;

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
  //Allow HTTPS without verification
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  
  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    fprintf(stderr, "curl POST failed: %s\n", curl_easy_strerror(res));
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
}

// Wall bitmask for each cell
enum { WALL_N = 1, WALL_E = 2, WALL_S = 4, WALL_W = 8 };

typedef struct {
  uint8_t walls;
  bool visited;
} Cell;

static Cell g[MAZE_H][MAZE_W];

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

// Player / goal rendering
static void draw_player_goal(SDL_Renderer* r, int px, int py) {
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
  return true;
}

static void regenerate(int* px, int* py, SDL_Window* win) {
  maze_init();
  maze_generate(0, 0);
  *px = 0; *py = 0;
  g_move_sequence = 0; // Reset move sequence on maze regeneration
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

  // POST to server
  post_json_to_server(json);
}

// Handle joystick/controller axis input + emit telemetry on move
// Returns true after goal reached
static bool handle_joystick_axis(int axis, Sint16 value, int* px, int* py, bool* joy_moved_x, bool* joy_moved_y, bool* won, SDL_Window* win) {
  bool moved = false;

  // X-axis (axis 0 for joystick, SDL_CONTROLLER_AXIS_LEFTX for controller)
  if (axis == 0) {
    if (value < -JOYSTICK_DEADZONE && !*joy_moved_x) {
      moved = try_move(px, py, -1, 0);
      *joy_moved_x = true;
    } else if (value > JOYSTICK_DEADZONE && !*joy_moved_x) {
      moved = try_move(px, py, 1, 0);
      *joy_moved_x = true;
    } else if (value > -JOYSTICK_DEADZONE && value < JOYSTICK_DEADZONE) {
      *joy_moved_x = false;
    }
  }

  // Y-axis (axis 1 for joystick, SDL_CONTROLLER_AXIS_LEFTY for controller)
  if (axis == 1) {
    if (value < -JOYSTICK_DEADZONE && !*joy_moved_y) {
      moved = try_move(px, py, 0, -1);
      *joy_moved_y = true;
    } else if (value > JOYSTICK_DEADZONE && !*joy_moved_y) {
      moved = try_move(px, py, 0, 1);
      *joy_moved_y = true;
    } else if (value > -JOYSTICK_DEADZONE && value < JOYSTICK_DEADZONE) {
      *joy_moved_y = false;
    }
  }

  if (moved) {
    g_move_sequence++;
    bool goal_reached = (*px == MAZE_W - 1 && *py == MAZE_H - 1);
    output_json_telemetry("joystick", *px, *py, goal_reached);

    if (goal_reached) {
      *won = true;
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

  // Generate session ID for telemetry
  generate_session_id();

  // For testing w/ PS5 controller
  //SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
  SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");

  // Initialize both VIDEO and JOYSTICK + GAMECONTROLLER subsystems
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
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

  SDL_Window* win = SDL_CreateWindow(
    "SDL2 Maze - Reach the green goal (R to regenerate)",
    SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
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
          regenerate(&px, &py, win);
          won = false;
        }

        if (!won) {
          bool moved = false;
          if (k == SDLK_UP || k == SDLK_w)          moved = try_move(&px, &py, 0, -1);
          else if (k == SDLK_RIGHT || k == SDLK_d)  moved = try_move(&px, &py, 1, 0);
          else if (k == SDLK_DOWN || k == SDLK_s)   moved = try_move(&px, &py, 0, 1);
          else if (k == SDLK_LEFT || k == SDLK_a)   moved = try_move(&px, &py, -1, 0);

          if (moved) {
            g_move_sequence++;
            bool goal_reached = (px == MAZE_W - 1 && py == MAZE_H - 1);
            output_json_telemetry("keyboard", px, py, goal_reached);

            if (goal_reached) {
              won = true;
              SDL_SetWindowTitle(win, "You win! Press R to regenerate, Esc to quit");
            }
          }
        }
      }

      // Handle game controller axis motion (recognized controllers)
      if (e.type == SDL_CONTROLLERAXISMOTION && !won) {
        int axis = (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) ? 0 : (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) ? 1 : -1;
        if (axis >= 0) {
          handle_joystick_axis(axis, e.caxis.value, &px, &py, &joy_moved_x, &joy_moved_y, &won, win);
        }
      }

      // Fallback (raw joystick axis motion)
      if (e.type == SDL_JOYAXISMOTION && !won && joystick) {
        handle_joystick_axis(e.jaxis.axis, e.jaxis.value, &px, &py, &joy_moved_x, &joy_moved_y, &won, win);
      }
    }

    draw_maze(r);
    draw_player_goal(r, px, py);

    SDL_RenderPresent(r);
  }

  if (controller) SDL_GameControllerClose(controller);
  if (joystick) SDL_JoystickClose(joystick);
  SDL_DestroyRenderer(r);
  SDL_DestroyWindow(win);
  SDL_Quit();
  curl_global_cleanup();
  return 0;
}
