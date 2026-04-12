import importlib.util
import sys
import types
from pathlib import Path

import numpy as np
import pytest


class FakeCFunction:
    def __init__(self, func):
        self.func = func
        self.argtypes = None
        self.restype = None

    def __call__(self, *args, **kwargs):
        return self.func(*args, **kwargs)


class FakeMazeLib:
    def __init__(self):
        self.wall_grid = np.array(
            [
                [1, 2, 3],
                [4, 5, 6],
            ],
            dtype=np.uint8,
        )
        self.dist_map = np.array(
            [
                [7, 6, 5],
                [4, 3, 0],
            ],
            dtype=np.int32,
        )
        self.astar_path = [(1, 0), (2, 0), (2, 1)]
        self.create_calls = []
        self.free_calls = []
        self.can_move_calls = []
        self.distance_calls = []
        self.astar_calls = []
        self.fail_create = False
        self.pointers = {}

        self.maze_create = FakeCFunction(self._maze_create)
        self.maze_free = FakeCFunction(self._maze_free)
        self.maze_width = FakeCFunction(lambda ptr: self.pointers[id(ptr)]["width"])
        self.maze_height = FakeCFunction(lambda ptr: self.pointers[id(ptr)]["height"])
        self.maze_get_walls = FakeCFunction(self._maze_get_walls)
        self.maze_get_wall_grid = FakeCFunction(self._maze_get_wall_grid)
        self.maze_can_move = FakeCFunction(self._maze_can_move)
        self.maze_compute_distance_map = FakeCFunction(self._maze_compute_distance_map)
        self.maze_astar = FakeCFunction(self._maze_astar)

    def _normalize_seed(self, seed):
        return seed.value if hasattr(seed, "value") else seed

    def _maze_create(self, width, height, seed):
        normalized_seed = self._normalize_seed(seed)
        self.create_calls.append((width, height, normalized_seed))
        if self.fail_create:
            return None
        ptr = object()
        self.pointers[id(ptr)] = {"width": width, "height": height, "seed": normalized_seed}
        return ptr

    def _maze_free(self, ptr):
        self.free_calls.append(ptr)
        self.pointers.pop(id(ptr), None)

    def _maze_get_walls(self, ptr, x, y):
        return int(self.wall_grid[y, x])

    def _maze_get_wall_grid(self, ptr, out_ptr):
        arr = np.ctypeslib.as_array(out_ptr, shape=(self.wall_grid.size,))
        arr[:] = self.wall_grid.reshape(-1)

    def _maze_can_move(self, ptr, x, y, dx, dy):
        self.can_move_calls.append((x, y, dx, dy))
        return 1 if (x, y, dx, dy) == (0, 0, 1, 0) else 0

    def _maze_compute_distance_map(self, ptr, gx, gy, out_ptr):
        self.distance_calls.append((gx, gy))
        arr = np.ctypeslib.as_array(out_ptr, shape=(self.dist_map.size,))
        arr[:] = self.dist_map.reshape(-1)
        return int(self.dist_map[0, 0])

    def _maze_astar(self, ptr, sx, sy, gx, gy, px, py):
        self.astar_calls.append((sx, sy, gx, gy))
        for index, (x, y) in enumerate(self.astar_path):
            px[index] = x
            py[index] = y
        return len(self.astar_path)


@pytest.fixture
def maze_bridge_module(monkeypatch):
    fake_lib = FakeMazeLib()
    module_path = Path(__file__).resolve().parents[3] / "maze" / "dagger" / "maze_bridge.py"
    module_name = "maze_bridge_under_test"
    spec = importlib.util.spec_from_file_location(module_name, module_path)
    module = importlib.util.module_from_spec(spec)

    monkeypatch.setattr(Path, "exists", lambda self: True)
    monkeypatch.setattr("ctypes.CDLL", lambda path: fake_lib)
    sys.modules[module_name] = module
    try:
        spec.loader.exec_module(module)
        yield module, fake_lib
    finally:
        sys.modules.pop(module_name, None)


# B2-13
def test_import_raises_when_library_is_missing(monkeypatch):
    module_path = Path(__file__).resolve().parents[3] / "maze" / "dagger" / "maze_bridge.py"
    module_name = "maze_bridge_missing_lib_test"
    spec = importlib.util.spec_from_file_location(module_name, module_path)
    module = importlib.util.module_from_spec(spec)
    monkeypatch.setattr(Path, "exists", lambda self: False)
    sys.modules[module_name] = module
    try:
        with pytest.raises(FileNotFoundError):
            spec.loader.exec_module(module)
    finally:
        sys.modules.pop(module_name, None)


# B2-14
def test_maze_handle_init_raises_when_create_fails(maze_bridge_module):
    module, fake_lib = maze_bridge_module
    fake_lib.fail_create = True

    with pytest.raises(RuntimeError):
        module.MazeHandle(3, 2, 99)


# B2-15
def test_maze_handle_close_is_idempotent(maze_bridge_module):
    module, fake_lib = maze_bridge_module
    handle = module.MazeHandle(3, 2, 42)
    ptr = handle._ptr

    handle.close()
    handle.close()

    assert fake_lib.create_calls == [(3, 2, 42)]
    assert fake_lib.free_calls == [ptr]
    assert handle._ptr is None


# B2-16
def test_maze_handle_context_manager_closes_pointer(maze_bridge_module):
    module, fake_lib = maze_bridge_module

    with module.MazeHandle(3, 2, 7) as handle:
        ptr = handle._ptr
        assert ptr is not None

    assert fake_lib.free_calls == [ptr]
    assert handle._ptr is None


# B2-17
def test_get_wall_grid_returns_expected_shape_dtype_and_content(maze_bridge_module):
    module, _ = maze_bridge_module
    handle = module.MazeHandle(3, 2, 1)

    try:
        walls = handle.get_wall_grid()
    finally:
        handle.close()

    assert walls.shape == (2, 3)
    assert walls.dtype == np.uint8
    assert np.array_equal(walls, np.array([[1, 2, 3], [4, 5, 6]], dtype=np.uint8))


# B2-18
def test_can_move_returns_boolean_value(maze_bridge_module):
    module, fake_lib = maze_bridge_module
    handle = module.MazeHandle(3, 2, 1)

    try:
        assert handle.can_move(0, 0, 1, 0) is True
        assert handle.can_move(0, 0, 0, 1) is False
    finally:
        handle.close()

    assert fake_lib.can_move_calls == [(0, 0, 1, 0), (0, 0, 0, 1)]


# B2-19
def test_compute_distance_map_returns_expected_shape_dtype_and_content(maze_bridge_module):
    module, fake_lib = maze_bridge_module
    handle = module.MazeHandle(3, 2, 5)

    try:
        dist = handle.compute_distance_map(2, 1)
    finally:
        handle.close()

    assert fake_lib.distance_calls == [(2, 1)]
    assert dist.shape == (2, 3)
    assert dist.dtype == np.int32
    assert np.array_equal(dist, np.array([[7, 6, 5], [4, 3, 0]], dtype=np.int32))


# B2-20
def test_astar_returns_list_of_coordinate_tuples(maze_bridge_module):
    module, fake_lib = maze_bridge_module
    handle = module.MazeHandle(3, 2, 10)

    try:
        path = handle.astar(0, 0, 2, 1)
    finally:
        handle.close()

    assert fake_lib.astar_calls == [(0, 0, 2, 1)]
    assert path == [(1, 0), (2, 0), (2, 1)]


# B2-21
def test_generate_training_instance_uses_default_goal_when_not_provided(maze_bridge_module):
    module, fake_lib = maze_bridge_module

    handle, walls, dist = module.generate_training_instance(width=3, height=2, seed=12)
    try:
        assert fake_lib.create_calls[-1] == (3, 2, 12)
        assert fake_lib.distance_calls[-1] == (2, 1)
        assert walls.shape == (2, 3)
        assert dist.shape == (2, 3)
    finally:
        handle.close()


# B2-22
def test_generate_training_instance_uses_explicit_goal_when_provided(maze_bridge_module):
    module, fake_lib = maze_bridge_module

    handle, _, _ = module.generate_training_instance(width=3, height=2, seed=12, goal_x=1, goal_y=0)
    try:
        assert fake_lib.distance_calls[-1] == (1, 0)
    finally:
        handle.close()
