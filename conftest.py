from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parent
MAZE_DIR = ROOT / "maze"

if str(MAZE_DIR) not in sys.path:
    sys.path.insert(0, str(MAZE_DIR))
