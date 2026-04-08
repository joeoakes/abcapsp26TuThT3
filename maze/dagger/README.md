# Imitation Learning Maze Navigation

---

### Setup

Initialize a venv and install pytorch and numpy.
Use CUDA if you can: `pip3 install torch torchvision --index-url https://download.pytorch.org/whl/cu128`

### Training

```bash
python3 -m dagger.train --checkpoint-dir checkpoints
```

### Evaluation

```bash
python3 -m dagger.evaluate --model checkpoints/dagger_best.pt
```