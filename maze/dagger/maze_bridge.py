"""
Python ctypes wrapper around libmaze.so

Exposes an interface for training:
    maze, dist_map = generate_training_instance(width, height, seed)
"""

import ctypes
import numpy as np
from pathlib import Path

# Load shared library

_LIB_PATH = Path(__file__).resolve().parent.parent / "libmaze.so"

if not _LIB_PATH.exists():
    raise FileNotFoundError(f"libmaze.so not found at {_LIB_PATH}. Run 'make libmaze.so' in the maze/ directory first.")

_lib = ctypes.CDLL(str(_LIB_PATH))

# ctypes prototypes

# Opaque pointer
class _MazeInstance(ctypes.Structure):
    pass

_MazePtr = ctypes.POINTER(_MazeInstance)

_lib.maze_create.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_uint32]
_lib.maze_create.restype = _MazePtr

_lib.maze_free.argtypes = [_MazePtr]
_lib.maze_free.restype = None

_lib.maze_width.argtypes = [_MazePtr]
_lib.maze_width.restype = ctypes.c_int

_lib.maze_height.argtypes = [_MazePtr]
_lib.maze_height.restype = ctypes.c_int

_lib.maze_get_walls.argtypes = [_MazePtr, ctypes.c_int, ctypes.c_int]
_lib.maze_get_walls.restype = ctypes.c_uint8

_lib.maze_get_wall_grid.argtypes = [_MazePtr, ctypes.POINTER(ctypes.c_uint8)]
_lib.maze_get_wall_grid.restype = None

_lib.maze_can_move.argtypes = [_MazePtr, ctypes.c_int, ctypes.c_int,
                                ctypes.c_int, ctypes.c_int]
_lib.maze_can_move.restype = ctypes.c_int

_lib.maze_compute_distance_map.argtypes = [
    _MazePtr, ctypes.c_int, ctypes.c_int,
    ctypes.POINTER(ctypes.c_int32),
]
_lib.maze_compute_distance_map.restype = ctypes.c_int

_lib.maze_astar.argtypes = [
    _MazePtr, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    ctypes.POINTER(ctypes.c_int), ctypes.POINTER(ctypes.c_int),
]
_lib.maze_astar.restype = ctypes.c_int


# High-level wrapper

# Wall bitmask constants
WALL_N = 1
WALL_E = 2
WALL_S = 4
WALL_W = 8


class MazeHandle:
    """RAII wrapper around a C MazeInstance pointer."""

    def __init__(self, width: int, height: int, seed: int):
        self._ptr = _lib.maze_create(width, height, ctypes.c_uint32(seed))
        if not self._ptr:
            raise RuntimeError(f"maze_create({width}, {height}, {seed}) failed")
        self.width = width
        self.height = height

    def close(self):
        if self._ptr:
            _lib.maze_free(self._ptr)
            self._ptr = None

    def __del__(self):
        self.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def get_wall_grid(self) -> np.ndarray:
        """Return (H, W) uint8 array of wall bitmasks."""
        buf = np.empty(self.height * self.width, dtype=np.uint8)
        _lib.maze_get_wall_grid(self._ptr, buf.ctypes.data_as(ctypes.POINTER(ctypes.c_uint8)))
        return buf.reshape(self.height, self.width)

    def can_move(self, x: int, y: int, dx: int, dy: int) -> bool:
        return bool(_lib.maze_can_move(self._ptr, x, y, dx, dy))

    def compute_distance_map(self, gx: int, gy: int) -> np.ndarray:
        """Return (H, W) int32 array of BFS distances to (gx, gy). -1 = unreachable."""
        buf = np.empty(self.height * self.width, dtype=np.int32)
        _lib.maze_compute_distance_map(
            self._ptr, gx, gy,
            buf.ctypes.data_as(ctypes.POINTER(ctypes.c_int32)),
        )
        return buf.reshape(self.height, self.width)

    def astar(self, sx: int, sy: int, gx: int, gy: int):
        """Return list of (x, y) tuples representing the A* path (excl. start, incl. goal)."""
        max_len = self.width * self.height
        px = (ctypes.c_int * max_len)()
        py = (ctypes.c_int * max_len)()
        length = _lib.maze_astar(self._ptr, sx, sy, gx, gy, px, py)
        return [(px[i], py[i]) for i in range(length)]


def generate_training_instance(width: int = 21, height: int = 15, seed: int = 0, goal_x: int = -1, goal_y: int = -1):
    """
    Generate a maze and its BFS distance map.

    Returns:
        handle: MazeHandle (keep alive while using wall_grid / dist_map)
        wall_grid: (H, W) uint8 array of wall bitmasks
        dist_map: (H, W) int32 array of BFS distances to goal (-1 = unreachable)
    """
    handle = MazeHandle(width, height, seed)
    if goal_x < 0:
        goal_x = width - 1
    if goal_y < 0:
        goal_y = height - 1
    wall_grid = handle.get_wall_grid()
    dist_map = handle.compute_distance_map(goal_x, goal_y)
    return handle, wall_grid, dist_map
