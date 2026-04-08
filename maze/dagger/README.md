# Imitation Learning Maze Navigation

---

## Setup

Initialize a venv and install pytorch and numpy.

Use CUDA if you can: `pip3 install torch torchvision --index-url https://download.pytorch.org/whl/cu128`

## Training

```bash
python3 -m dagger.train --checkpoint-dir checkpoints
```

## Evaluation

```bash
python3 -m dagger.evaluate --model checkpoints/dagger_best.pt --num-mazes 10000
python3 -m dagger.evaluate --model checkpoints/dagger_best.pt --num-mazes 10000 --epsilon 0.05
```

### Results:

```bash
Loaded model from checkpoints/dagger_best.pt
  Device: cuda
  Episodes trained: 3500
  Train steps: 160328
  Eval epsilon: 0.0000

============================================================
EVALUATION RESULTS (10000 test mazes)
============================================================
  Epsilon:             0.0
  Success rate:        88.0%
  Avg path length:     166.3
  Avg path efficiency (successes only): 1.00x optimal

  Failure analysis:
    looping   : 1067 (88.7%)
    timeout   :  136 (11.3%)

  Total eval time:     568.40s
  Throughput:          17.59 mazes/sec
```

```bash
Loaded model from checkpoints/dagger_best.pt
  Device: cuda
  Episodes trained: 3500
  Train steps: 160328
  Eval epsilon: 0.0500

============================================================
EVALUATION RESULTS (10000 test mazes)
============================================================
  Epsilon:             0.05
  Success rate:        92.1%
  Avg path length:     165.0
  Avg path efficiency (successes only): 1.10x optimal

  Failure analysis:
    looping   :  653 (82.6%)
    timeout   :  138 (17.4%)

  Total eval time:     535.02s
  Throughput:          18.69 mazes/sec
```