# Unit Testing Plan

## Purpose

This plan defines a practical, repo-specific unit testing strategy for:

- Python components using `pytest`
- C components using `Unity`

The goal is to prioritize deterministic, fast, isolated tests first, then expand coverage by extracting pure logic out of the larger runtime-oriented modules.

## Testing Goals

- Protect core maze generation and pathfinding logic from regressions
- Validate robot command translation and HTTP request handling in Python without requiring ROS2 hardware
- Validate C helper logic without depending on SDL2 windows, live TLS sessions, MongoDB, or Redis
- Separate unit tests from integration/system tests so failures are easier to diagnose
- Establish a path toward CI-safe, repeatable test execution

## Test Pyramid for This Repo

### Unit tests

Fast, deterministic, no network, no GUI, no real database, no real robot.

Primary targets:

- `maze/dagger/*.py`
- `robot/robot_bridge.py`
- `maze/maze_lib.c`
- pure/helper logic extracted from `maze/maze_sdl2.c`, `http/maze_http_mongo.c`, and `https_final/maze_https_mongo.c`

### Integration tests

Use real shared library, local HTTP server, TLS certs, or containerized MongoDB/Redis.

Primary targets:

- `maze/dagger/maze_bridge.py` loading `libmaze.so`
- HTTP and HTTPS request paths end to end
- robot bridge over HTTPS with mocked ROS2 publisher or local ROS2 environment

### System tests

Full stack with SDL2 maze client, HTTPS services, Redis, MongoDB, and robot bridge.

## Recommended Repository Layout

### Python

Recommended layout:

- `tests/python/conftest.py`
- `tests/python/robot/test_robot_bridge.py`
- `tests/python/maze_dagger/test_maze_bridge.py`
- `tests/python/maze_dagger/test_maze_env.py`
- `tests/python/maze_dagger/test_agent.py`
- `tests/python/maze_dagger/test_smoke_bridge_conversion.py`

Notes:

- Convert the current `maze/test_bridge.py` smoke script into one or more `pytest` tests instead of relying on print-based checks.
- Keep unit tests separate from any future integration tests, for example under `tests/integration/`.

### C

Recommended layout:

- `tests/c/unity/` for vendored Unity sources
- `tests/c/test_maze_lib.c`
- `tests/c/test_maze_logic.c`
- `tests/c/test_http_helpers.c`
- `tests/c/test_https_helpers.c`
- `tests/c/support/` for fakes, test data, and adapters

Notes:

- `maze/maze_lib.c` is already a good unit-test target.
- The larger runtime files should not be tested as monoliths first. Instead, extract pure logic into small helper modules and test those with Unity.

## Python Plan with Pytest

## Pytest Tooling

Recommended packages:

- `pytest`
- `pytest-cov`
- `pytest-mock`

Optional:

- `hypothesis` for property-based checks on maze invariants
- `pytest-xdist` for parallel execution

## Python Test Categories

### 1. `robot/robot_bridge.py`

This file contains a mix of pure logic, side-effectful movement execution, TLS setup, and HTTP handling. Most of it is unit testable with mocking.

#### High-priority unit tests

- `build_tls_context()`
  - creates a server `SSLContext`
  - enforces minimum TLS version >= 1.2
  - loads the configured certificate chain
  - loads the configured CA file
  - sets `verify_mode` to require client certificates

- `_publish_twist()`
  - returns cleanly when `_cmd_pub` is `None`
  - publishes exactly one `Twist` with the expected `linear.x` and `angular.z`

- `_stop()`
  - publishes a zero-velocity command

- `execute_action()`
  - maps `forward`, `backward`, `turn_left`, `turn_right`, and `stop` to the correct velocity pairs
  - ignores unknown actions without publishing
  - uses `MOVE_DURATION` for linear movement
  - computes turn duration from `TURN_SPEED` and `TURN_ANGLE_MULT`
  - inserts `TURN_MOVE_DELAY` when a forward/backward command immediately follows a turn
  - repeatedly publishes movement commands during the action window
  - publishes a stop at the end of non-stop actions
  - updates `_last_action` correctly

#### Edge-case unit tests

- `execute_action()` with very small mocked durations so tests remain fast
- `execute_action()` when `TURN_SPEED` is negative or positive
- `execute_action()` lock behavior under concurrent calls using mocked threads or direct sequential simulation

#### HTTP handler unit tests

For `RobotHandler.do_POST()` use a lightweight fake request/handler harness or instantiate the handler around in-memory streams.

Test cases:

- non-`/robot` path returns 404
- empty request body returns 400
- malformed JSON returns 400
- missing `action` field returns 400
- valid request starts a background thread targeting `execute_action`
- valid request returns `200` and JSON body immediately

#### Not unit-test priority

- `main()`
- actual ROS2 spinning
- real TLS socket wrapping
- real HTTPS server behavior

Those are better covered by integration tests.

### 2. `maze/dagger/maze_bridge.py`

This module is a thin ctypes wrapper around `libmaze.so`.

#### High-priority unit tests

- `MazeHandle.__init__()` raises `RuntimeError` when `maze_create` fails
- `close()` is idempotent
- context manager behavior calls `close()` on exit
- `get_wall_grid()` returns an array with shape `(height, width)` and dtype `uint8`
- `compute_distance_map()` returns shape `(height, width)` and dtype `int32`
- `astar()` returns a list of `(x, y)` tuples of expected length
- `generate_training_instance()` defaults goal to bottom-right when `goal_x` and `goal_y` are omitted

#### Recommended approach

Use two layers of tests:

- mock-based unit tests by monkeypatching `_lib`
- a small integration-style test set that uses the real `libmaze.so`

### 3. `maze/dagger/maze_env.py`

This file is highly testable and should get broad unit coverage.

#### Constructor and reset behavior

- invalid `observation_mode` raises `ValueError`
- `observation_space_shape` is correct for `local` and `global`
- `action_space_n` is `4`
- `reset(seed=...)` is deterministic
- fixed seed mode reuses the configured seed across resets
- random seed mode produces valid state even when the actual maze changes
- `reset()` initializes:
  - agent at `(0, 0)`
  - goal at bottom-right
  - `steps == 0`
  - initial visit count at the start cell

#### Step behavior

- moving into an open neighbor updates the agent position
- moving into a blocked direction leaves position unchanged
- `steps` increments every call
- reaching the goal sets `done = True` and `info["success"] = True`
- exceeding `max_steps` ends the episode

#### Observation tests

For local observations:

- output shape is `(obs_dim,)`
- out-of-bounds patch cells are treated as fully walled
- visit patch reflects previously visited cells
- goal direction and distance features are present and normalized

For global observations:

- output shape is `(7, height, width)`
- wall channels reflect `WALL_N/E/S/W`
- visited channel marks visited cells
- agent channel is one-hot at the agent location
- goal channel is one-hot at the goal location

#### Expert helper tests

- `optimal_path_length()` matches `dist_map[0, 0]`
- `optimal_action()` returns `None` at the goal
- `optimal_action()` chooses a legal move that strictly reduces BFS distance
- `render_ascii()` includes agent marker, goal marker, and border structure

### 4. `maze/dagger/agent.py`

This file should have strong unit coverage because it contains core ML utility logic.

#### `PolicyNetwork`

- vector observation shape produces logits with shape `(batch, n_actions)`
- spatial observation shape produces logits with shape `(batch, n_actions)`
- single unbatched state is accepted and internally batched
- invalid observation shape raises `ValueError`

#### `ExpertBuffer`

- `push()` increases size
- `sample()` returns arrays of expected shape and dtype
- capacity limit evicts oldest entries as expected
- `__len__()` reports current buffer size

#### `DAggerAgent`

- integer `obs_shape` is normalized to a tuple
- `select_action(..., greedy=True)` ignores epsilon exploration
- `select_action(..., greedy=False)` explores when random draw is below epsilon
- `store_expert_label()` appends to the buffer
- `train_step()` returns `None` when buffer is too small
- `train_step()` returns a numeric loss when enough samples exist
- `train_step()` increments `train_steps`
- `end_episode()` decays epsilon but not below `epsilon_end`
- `save()` and `load()` round-trip model state and metadata

#### Testing notes

- Set deterministic seeds for `random`, `numpy`, and `torch`
- Mock or monkeypatch `random.random()` and `random.randrange()` where exact exploration behavior matters
- Keep training minibatches tiny to ensure tests run quickly

## C Plan with Unity

## Unity Tooling

Use `Unity` as the C assertion framework.

Recommended execution options:

- plain Unity with a small custom `Makefile`, or
- Unity plus `CMock/Ceedling` if you want easier mocks for external dependencies

For this repo, the best path is:

- start with plain Unity for `maze_lib.c`
- add test seams and optional mocking support for the HTTP/HTTPS modules later

## C Test Categories

### 1. `maze/maze_lib.c`

This is the highest-value C unit-test target and should be implemented first.

#### Constructor and basic API

- `maze_create()` returns `NULL` for invalid dimensions
  - width or height < 2
  - width > `MAZE_MAX_W`
  - height > `MAZE_MAX_H`
- `maze_create()` returns non-`NULL` for valid dimensions
- `maze_width()` and `maze_height()` return expected values
- `maze_free(NULL)` is safe
- `maze_get_walls()` returns `0xF` for out-of-bounds coordinates

#### Determinism and structural invariants

- same seed produces identical wall grids
- adjacent cells agree on shared walls
  - if cell A has east wall open, neighbor B has west wall open
  - same for north/south
- every cell wall mask stays within the four defined bits
- generated maze is fully reachable from the goal according to `maze_compute_distance_map()`

#### Movement tests

- `maze_can_move()` returns `0` for invalid source coordinates
- `maze_can_move()` returns `0` for moves leaving bounds
- `maze_can_move()` agrees with wall bitmasks for each legal direction

#### Distance map tests

- invalid goal or null output returns `-1`
- goal cell distance is `0`
- all reachable cells get non-negative distance values
- returned distance for `(0, 0)` is non-negative in a generated perfect maze
- neighbor distances differ by at most `1` where movement is legal

#### A* tests

- invalid inputs return `0`
- start == goal returns `0`
- returned path length equals BFS distance from start to goal
- each returned step is adjacent to the previous one
- each returned step is legal according to `maze_can_move()`
- final step reaches the goal

### 2. `maze/maze_sdl2.c`

This file currently mixes SDL2 rendering, libcurl networking, queueing, telemetry formatting, mission stats, robot command translation, and maze logic. It is not an ideal direct unit-test target as-is.

#### Recommended unit-test strategy

Extract pure logic into small helper modules before writing many Unity tests.

Recommended extractions:

- `maze_logic.c/.h`
  - `robot_heading_from_move()`
  - movement-to-heading translation
  - `update_move_counters()`
  - pure path helpers if reused

- `telemetry_format.c/.h`
  - JSON payload formatting for movement telemetry
  - mission summary formatting
  - session/summary field population that does not require network I/O

- `queue_logic.c/.h`
  - bounded queue behavior if queue operations are to be tested independently of SDL mutex/cond types

#### High-value tests after extraction

- heading mapping from `(dx, dy)` to `N/E/S/W`
- robot turn sequencing for heading changes
- movement counter increments for left/right/up/down or forward/reverse semantics used by mission summary
- telemetry JSON contains required keys and values
- mission summary defaults and totals are computed correctly
- invalid movement deltas are ignored or handled predictably

#### Keep out of unit scope

- SDL rendering functions
- live curl POSTs
- actual worker threads using SDL synchronization primitives

Those belong in integration tests.

### 3. `http/maze_http_mongo.c`

This file contains some immediately testable helpers and some request/database logic that should be isolated before deep unit testing.

#### Good unit-test targets now

- `getenv_or()`
- `parse_long_clamped()`
- `strbuf_append()` and `strbuf_free()`
- `bodybuf_append()` and `bodybuf_free()`
- `json_to_bson_with_received_at()`
  - valid JSON parses
  - invalid JSON fails
  - `received_at` field is added
  - timestamp format is ISO-8601-like UTC

#### Better after seam extraction

Extract wrappers around:

- Mongo insert/query operations
- libmicrohttpd request accessors
- response building

Then unit test:

- POST validation logic for empty body and invalid JSON
- GET `/moves` parameter parsing and clamping
- response JSON truncation behavior
- session filtering logic

#### Keep out of unit scope initially

- real `mongoc_collection_insert_one`
- real `mongoc_collection_find_with_opts`
- full libmicrohttpd request lifecycle

### 4. `https_final/maze_https_mongo.c`

This file is the most complex C server and should be approached incrementally.

#### Good unit-test targets now

- `parse_long_clamped()`
- `strbuf_append()` and `strbuf_free()`
- `body_append()`
- `read_file()` success/failure behavior using temporary files
- `get_utc_iso8601()` output format
- `now_epoch_seconds()` sanity check range
- `get_utf8_or()`
- `get_i64_or()`
- `get_double_or()`

#### Better after seam extraction

Extract the following into testable helpers:

- mission document normalization and defaulting
- Redis key construction
- mission summary field derivation
- certificate verification adapter boundary
- route dispatch decisions from `request_handler()`

Then unit test:

- missing `mission_id` is rejected
- duration is clamped to non-negative
- `moves_total` defaults to the sum of component move counts when absent
- default `robot_id`, `mission_type`, `mission_result`, and `abort_reason` are applied correctly
- route matching sends `/health`, `/moves`, `/mission`, `/move` to the correct handler
- unknown routes return not found

#### Not first-wave unit-test targets

- `verify_client_cert()` against real GNUTLS sessions
- real Redis and MongoDB calls
- full TLS daemon startup

These are better covered by integration tests.

## Testability Improvements Recommended Before Heavy C Coverage

The biggest obstacle to Unity coverage is that several C files are large monoliths with many `static` functions tied directly to external libraries.

Recommended refactor sequence:

1. Keep `maze/maze_lib.c` as the first fully unit-tested C module.
2. Extract pure logic from `maze/maze_sdl2.c` into helper modules.
3. Extract parsing, normalization, and formatting helpers from the HTTP/HTTPS servers.
4. Wrap external dependencies behind small adapters so they can be mocked or faked.

This gives fast coverage gains without destabilizing runtime behavior.

## Recommended Pytest Fixtures and Fakes

### Shared fixtures

- deterministic RNG fixture for `random`, `numpy`, and `torch`
- temporary checkpoint directory fixture for save/load tests
- fake publisher fixture for `_cmd_pub`
- monkeypatched `time.sleep` fixture to avoid real waiting
- monkeypatched `threading.Thread` fixture to assert background dispatch

### Maze fixtures

- real `MazeHandle` fixture using `libmaze.so`
- deterministic `MazeEnv` fixture with fixed seed
- optional mocked `_lib` fixture for pure wrapper tests

## Recommended Unity Support Utilities

- helper functions to compare arrays and path sequences
- helper to validate wall symmetry across the full maze grid
- temporary-file helper for `read_file()` tests
- BSON construction helpers for accessor tests in `https_final/maze_https_mongo.c`

## What Should Remain Integration Tests

The following should not be forced into unit tests:

- ROS2 publisher creation and node spinning
- SDL2 rendering correctness
- libcurl network calls
- libmicrohttpd daemon lifecycle
- GNUTLS certificate verification against live peer certs
- MongoDB and Redis connectivity

These should be covered by a smaller integration suite after the unit layer is stable.

## Coverage Targets

Suggested initial thresholds:

### Python

- `maze/dagger/*.py`: 85%+ line coverage
- `robot/robot_bridge.py`: 75%+ line coverage excluding `main()` and live server startup paths

### C

- `maze/maze_lib.c`: 90%+ function coverage
- extracted helper modules from `maze_sdl2.c`, `http`, and `https_final`: 80%+ function coverage

Do not gate CI on full monolithic server coverage before extraction work is done.

## Rollout Plan

### Phase 1: Quick wins

- convert `maze/test_bridge.py` into pytest tests
- add pytest coverage for:
  - `maze/dagger/maze_env.py`
  - `maze/dagger/agent.py`
  - mock-based tests for `robot/robot_bridge.py`
- add Unity tests for `maze/maze_lib.c`

### Phase 2: Improve seams

- extract pure helpers from `maze/maze_sdl2.c`
- extract request parsing and normalization helpers from the C servers
- add Unity tests for extracted helpers

### Phase 3: Add targeted integration tests

- real `libmaze.so` wrapper tests
- local HTTP server tests with temporary data stores or mocks
- TLS/robot bridge integration tests in a controlled environment

## Suggested CI Split

Run separate jobs:

- Python unit tests: `pytest`
- C unit tests: Unity test binary or Ceedling job
- optional integration tests only on demand or on protected branches

This avoids flaky failures from GUI, ROS2, DB, and TLS dependencies in the main unit-test path.

## Definition of Done

This plan is considered implemented when:

- `pytest` covers all Python modules with deterministic, isolated tests
- `maze/maze_lib.c` has a complete Unity suite
- major pure helpers from `maze_sdl2.c`, `http`, and `https_final` are extracted and covered by Unity
- integration-only concerns are explicitly separated from unit-test jobs
- CI runs unit tests consistently without requiring robot hardware, a GUI, or live infrastructure
