#include "unity.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "maze_lib.h"

void setUp(void) {}
void tearDown(void) {}

static void assert_same_wall_grid(MazeInstance* left, MazeInstance* right, int width, int height) {
    uint8_t left_grid[MAZE_MAX_W * MAZE_MAX_H];
    uint8_t right_grid[MAZE_MAX_W * MAZE_MAX_H];
    int total = width * height;

    memset(left_grid, 0, sizeof(left_grid));
    memset(right_grid, 0, sizeof(right_grid));
    maze_get_wall_grid(left, left_grid);
    maze_get_wall_grid(right, right_grid);

    for (int index = 0; index < total; index++) {
        TEST_ASSERT_EQUAL_UINT8(left_grid[index], right_grid[index]);
    }
}

static void assert_distance_gradient_is_consistent(MazeInstance* maze, const int32_t* dist) {
    const int dx[4] = {0, 1, 0, -1};
    const int dy[4] = {-1, 0, 1, 0};

    for (int y = 0; y < maze_height(maze); y++) {
        for (int x = 0; x < maze_width(maze); x++) {
            int current = dist[y * maze_width(maze) + x];
            int found_descending_neighbor = (current == 0);

            TEST_ASSERT_TRUE(current >= 0);
            for (int dir = 0; dir < 4; dir++) {
                int nx = x + dx[dir];
                int ny = y + dy[dir];
                if (!maze_can_move(maze, x, y, dx[dir], dy[dir])) {
                    continue;
                }

                TEST_ASSERT_TRUE(nx >= 0);
                TEST_ASSERT_TRUE(nx < maze_width(maze));
                TEST_ASSERT_TRUE(ny >= 0);
                TEST_ASSERT_TRUE(ny < maze_height(maze));

                {
                    int neighbor = dist[ny * maze_width(maze) + nx];
                    TEST_ASSERT_TRUE(neighbor >= 0);
                    TEST_ASSERT_TRUE(abs(current - neighbor) <= 1);
                    if (neighbor == current - 1) {
                        found_descending_neighbor = 1;
                    }
                }
            }

            TEST_ASSERT_TRUE(found_descending_neighbor);
        }
    }
}

/* B2-01 */
void test_maze_create_rejects_invalid_dimensions(void) {
    TEST_ASSERT_NULL(maze_create(1, 5, 7));
    TEST_ASSERT_NULL(maze_create(5, 1, 7));
    TEST_ASSERT_NULL(maze_create(MAZE_MAX_W + 1, 5, 7));
    TEST_ASSERT_NULL(maze_create(5, MAZE_MAX_H + 1, 7));
}

/* B2-02 */
void test_maze_create_returns_dimensions_for_valid_input(void) {
    MazeInstance* maze = maze_create(6, 4, 11);

    TEST_ASSERT_NOT_NULL(maze);
    TEST_ASSERT_EQUAL_INT(6, maze_width(maze));
    TEST_ASSERT_EQUAL_INT(4, maze_height(maze));

    maze_free(maze);
}

/* B2-03 */
void test_maze_free_accepts_null(void) {
    maze_free(NULL);
    TEST_ASSERT_TRUE(1);
}

/* B2-04 */
void test_maze_get_walls_returns_full_mask_out_of_bounds(void) {
    MazeInstance* maze = maze_create(4, 4, 3);

    TEST_ASSERT_NOT_NULL(maze);
    TEST_ASSERT_EQUAL_HEX8(0x0F, maze_get_walls(maze, -1, 0));
    TEST_ASSERT_EQUAL_HEX8(0x0F, maze_get_walls(maze, 0, -1));
    TEST_ASSERT_EQUAL_HEX8(0x0F, maze_get_walls(maze, 4, 0));
    TEST_ASSERT_EQUAL_HEX8(0x0F, maze_get_walls(maze, 0, 4));

    maze_free(maze);
}

/* B2-05 */
void test_same_seed_produces_identical_wall_grid(void) {
    MazeInstance* left = maze_create(8, 6, 42);
    MazeInstance* right = maze_create(8, 6, 42);

    TEST_ASSERT_NOT_NULL(left);
    TEST_ASSERT_NOT_NULL(right);
    assert_same_wall_grid(left, right, 8, 6);

    maze_free(left);
    maze_free(right);
}

/* B2-06 */
void test_wall_masks_and_shared_walls_are_consistent(void) {
    MazeInstance* maze = maze_create(7, 5, 9);

    TEST_ASSERT_NOT_NULL(maze);

    for (int y = 0; y < maze_height(maze); y++) {
        for (int x = 0; x < maze_width(maze); x++) {
            uint8_t walls = maze_get_walls(maze, x, y);
            TEST_ASSERT_EQUAL_HEX8((uint8_t)(walls & 0x0F), walls);

            if (x + 1 < maze_width(maze)) {
                uint8_t east_open = (uint8_t)((walls & WALL_E) == 0);
                uint8_t neighbor_walls = maze_get_walls(maze, x + 1, y);
                uint8_t west_open = (uint8_t)((neighbor_walls & WALL_W) == 0);
                TEST_ASSERT_EQUAL_UINT8(east_open, west_open);
            }

            if (y + 1 < maze_height(maze)) {
                uint8_t south_open = (uint8_t)((walls & WALL_S) == 0);
                uint8_t neighbor_walls = maze_get_walls(maze, x, y + 1);
                uint8_t north_open = (uint8_t)((neighbor_walls & WALL_N) == 0);
                TEST_ASSERT_EQUAL_UINT8(south_open, north_open);
            }
        }
    }

    maze_free(maze);
}

/* B2-07 */
void test_maze_can_move_matches_wall_bitmasks(void) {
    const int dx[4] = {0, 1, 0, -1};
    const int dy[4] = {-1, 0, 1, 0};
    const uint8_t mask[4] = {WALL_N, WALL_E, WALL_S, WALL_W};
    MazeInstance* maze = maze_create(6, 6, 14);

    TEST_ASSERT_NOT_NULL(maze);
    TEST_ASSERT_FALSE(maze_can_move(maze, -1, 0, 1, 0));
    TEST_ASSERT_FALSE(maze_can_move(maze, 0, 0, -1, 0));

    for (int y = 0; y < maze_height(maze); y++) {
        for (int x = 0; x < maze_width(maze); x++) {
            uint8_t walls = maze_get_walls(maze, x, y);
            for (int dir = 0; dir < 4; dir++) {
                int nx = x + dx[dir];
                int ny = y + dy[dir];
                int expected = 0;
                if (nx >= 0 && nx < maze_width(maze) && ny >= 0 && ny < maze_height(maze) && (walls & mask[dir]) == 0) {
                    expected = 1;
                }
                TEST_ASSERT_EQUAL_INT(expected, maze_can_move(maze, x, y, dx[dir], dy[dir]));
            }
        }
    }

    maze_free(maze);
}

/* B2-08 */
void test_distance_map_rejects_invalid_inputs(void) {
    int32_t dist[MAZE_MAX_W * MAZE_MAX_H];
    MazeInstance* maze = maze_create(5, 4, 18);

    TEST_ASSERT_EQUAL_INT(-1, maze_compute_distance_map(NULL, 0, 0, dist));
    TEST_ASSERT_NOT_NULL(maze);
    TEST_ASSERT_EQUAL_INT(-1, maze_compute_distance_map(maze, -1, 0, dist));
    TEST_ASSERT_EQUAL_INT(-1, maze_compute_distance_map(maze, 0, -1, dist));
    TEST_ASSERT_EQUAL_INT(-1, maze_compute_distance_map(maze, maze_width(maze), 0, dist));
    TEST_ASSERT_EQUAL_INT(-1, maze_compute_distance_map(maze, 0, maze_height(maze), dist));
    TEST_ASSERT_EQUAL_INT(-1, maze_compute_distance_map(maze, 0, 0, NULL));

    maze_free(maze);
}

/* B2-09 */
void test_distance_map_marks_goal_zero_and_all_cells_reachable(void) {
    int32_t dist[MAZE_MAX_W * MAZE_MAX_H];
    MazeInstance* maze = maze_create(6, 5, 21);
    int width;
    int height;
    int start_distance;

    TEST_ASSERT_NOT_NULL(maze);
    width = maze_width(maze);
    height = maze_height(maze);
    start_distance = maze_compute_distance_map(maze, width - 1, height - 1, dist);

    TEST_ASSERT_TRUE(start_distance >= 0);
    TEST_ASSERT_EQUAL_INT(0, dist[(height - 1) * width + (width - 1)]);

    for (int index = 0; index < width * height; index++) {
        TEST_ASSERT_TRUE(dist[index] >= 0);
    }

    maze_free(maze);
}

/* B2-10 */
void test_distance_map_neighbor_distances_are_locally_consistent(void) {
    int32_t dist[MAZE_MAX_W * MAZE_MAX_H];
    MazeInstance* maze = maze_create(6, 5, 27);
    int width;
    int height;

    TEST_ASSERT_NOT_NULL(maze);
    width = maze_width(maze);
    height = maze_height(maze);
    TEST_ASSERT_TRUE(maze_compute_distance_map(maze, width - 1, height - 1, dist) >= 0);

    assert_distance_gradient_is_consistent(maze, dist);

    maze_free(maze);
}

/* B2-11 */
void test_astar_path_is_legal_and_matches_distance_map_length(void) {
    int path_x[MAZE_MAX_W * MAZE_MAX_H];
    int path_y[MAZE_MAX_W * MAZE_MAX_H];
    int32_t dist[MAZE_MAX_W * MAZE_MAX_H];
    MazeInstance* maze = maze_create(7, 5, 33);
    int width;
    int height;
    int path_len;
    int prev_x = 0;
    int prev_y = 0;

    TEST_ASSERT_NOT_NULL(maze);
    width = maze_width(maze);
    height = maze_height(maze);
    TEST_ASSERT_TRUE(maze_compute_distance_map(maze, width - 1, height - 1, dist) >= 0);

    path_len = maze_astar(maze, 0, 0, width - 1, height - 1, path_x, path_y);

    TEST_ASSERT_EQUAL_INT(dist[0], path_len);
    TEST_ASSERT_TRUE(path_len > 0);

    for (int i = 0; i < path_len; i++) {
        int step_x = path_x[i];
        int step_y = path_y[i];
        int manhattan = abs(step_x - prev_x) + abs(step_y - prev_y);
        TEST_ASSERT_EQUAL_INT(1, manhattan);
        TEST_ASSERT_TRUE(maze_can_move(maze, prev_x, prev_y, step_x - prev_x, step_y - prev_y));
        prev_x = step_x;
        prev_y = step_y;
    }

    TEST_ASSERT_EQUAL_INT(width - 1, prev_x);
    TEST_ASSERT_EQUAL_INT(height - 1, prev_y);

    maze_free(maze);
}

/* B2-12 */
void test_astar_rejects_invalid_inputs_and_start_equals_goal(void) {
    int path_x[MAZE_MAX_W * MAZE_MAX_H];
    int path_y[MAZE_MAX_W * MAZE_MAX_H];
    MazeInstance* maze = maze_create(5, 4, 31);

    TEST_ASSERT_NOT_NULL(maze);
    TEST_ASSERT_EQUAL_INT(0, maze_astar(NULL, 0, 0, 1, 1, path_x, path_y));
    TEST_ASSERT_EQUAL_INT(0, maze_astar(maze, 0, 0, 1, 1, NULL, path_y));
    TEST_ASSERT_EQUAL_INT(0, maze_astar(maze, 0, 0, 1, 1, path_x, NULL));
    TEST_ASSERT_EQUAL_INT(0, maze_astar(maze, -1, 0, 1, 1, path_x, path_y));
    TEST_ASSERT_EQUAL_INT(0, maze_astar(maze, 0, -1, 1, 1, path_x, path_y));
    TEST_ASSERT_EQUAL_INT(0, maze_astar(maze, 0, 0, maze_width(maze), 1, path_x, path_y));
    TEST_ASSERT_EQUAL_INT(0, maze_astar(maze, 0, 0, 1, maze_height(maze), path_x, path_y));
    TEST_ASSERT_EQUAL_INT(0, maze_astar(maze, 2, 2, 2, 2, path_x, path_y));

    maze_free(maze);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_maze_create_rejects_invalid_dimensions);
    RUN_TEST(test_maze_create_returns_dimensions_for_valid_input);
    RUN_TEST(test_maze_free_accepts_null);
    RUN_TEST(test_maze_get_walls_returns_full_mask_out_of_bounds);
    RUN_TEST(test_same_seed_produces_identical_wall_grid);
    RUN_TEST(test_wall_masks_and_shared_walls_are_consistent);
    RUN_TEST(test_maze_can_move_matches_wall_bitmasks);
    RUN_TEST(test_distance_map_rejects_invalid_inputs);
    RUN_TEST(test_distance_map_marks_goal_zero_and_all_cells_reachable);
    RUN_TEST(test_distance_map_neighbor_distances_are_locally_consistent);
    RUN_TEST(test_astar_path_is_legal_and_matches_distance_map_length);
    RUN_TEST(test_astar_rejects_invalid_inputs_and_start_equals_goal);
    return UNITY_END();
}
