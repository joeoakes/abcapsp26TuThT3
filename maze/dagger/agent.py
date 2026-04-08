"""
CNN policy trained via Dataset Aggregation (DAgger):
    - MLP encoder for vector observations / CNN encoder for spatial observations
    - Expert buffer for oracle (state, action) pairs
    - Behavior cloning with CrossEntropyLoss
    - Epsilon-greedy exploration for DAgger rollouts
"""

import random
from collections import deque, namedtuple

import numpy as np
import torch
import torch.nn as nn
import torch.optim as optim

ExpertSample = namedtuple("ExpertSample", ("state", "action"))


class PolicyNetwork(nn.Module):
    """Policy network supporting vector or channel-first spatial observations."""

    def __init__(self, obs_shape: tuple[int, ...], n_actions: int, hidden: int = 256):
        super().__init__()
        self.obs_shape = tuple(obs_shape)

        if len(self.obs_shape) == 1:
            obs_dim = self.obs_shape[0]
            self.encoder = nn.Sequential(
                nn.Linear(obs_dim, hidden),
                nn.ReLU(),
                nn.Linear(hidden, hidden),
                nn.ReLU(),
                nn.Linear(hidden, hidden // 2),
                nn.ReLU(),
            )
            self.head = nn.Linear(hidden // 2, n_actions)
        elif len(self.obs_shape) == 3:
            channels, height, width = self.obs_shape
            self.encoder = nn.Sequential(
                nn.Conv2d(channels, 32, kernel_size=3, padding=1),
                nn.ReLU(),
                nn.Conv2d(32, 64, kernel_size=3, padding=1),
                nn.ReLU(),
                nn.Conv2d(64, 64, kernel_size=3, padding=1),
                nn.ReLU(),
                nn.Flatten(),
            )
            with torch.no_grad():
                feat_dim = self.encoder(torch.zeros(1, channels, height, width)).shape[1]
            self.head = nn.Sequential(
                nn.Linear(feat_dim, hidden),
                nn.ReLU(),
                nn.Linear(hidden, n_actions),
            )
        else:
            raise ValueError("obs_shape must have length 1 or 3")

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        if x.dim() == len(self.obs_shape):
            x = x.unsqueeze(0)
        if len(self.obs_shape) == 1:
            x = x.view(x.size(0), -1)
        features = self.encoder(x)
        return self.head(features)


class ExpertBuffer:
    """Fixed-size FIFO buffer for (state, action) expert labels."""

    def __init__(self, capacity: int = 200_000):
        self.buffer = deque(maxlen=capacity)

    def push(self, state, action):
        self.buffer.append(ExpertSample(state, action))

    def sample(self, batch_size: int):
        batch = random.sample(self.buffer, batch_size)
        states = np.stack([s.state for s in batch])
        actions = np.array([s.action for s in batch], dtype=np.int64)
        return states, actions

    def __len__(self):
        return len(self.buffer)


class DAggerAgent:
    """
    DAgger imitation learning agent with epsilon-greedy exploration.

    Parameters:
        obs_shape: observation shape
        n_actions: number of discrete actions
        lr: learning rate
        epsilon_start: initial exploration rate for DAgger rollouts
        epsilon_end: minimum exploration rate
        epsilon_decay: multiplicative decay per episode
        batch_size: minibatch size for training
        expert_buffer_capacity: expert buffer size
        hidden: hidden layer width
        device: 'cpu' or 'cuda'
    """

    def __init__(
        self,
        obs_shape: int | tuple[int, ...],
        n_actions: int,
        lr: float = 1e-3,
        epsilon_start: float = 1.0,
        epsilon_end: float = 0.05,
        epsilon_decay: float = 0.995,
        batch_size: int = 64,
        expert_buffer_capacity: int = 200_000,
        hidden: int = 256,
        device: str = "cpu",
    ):
        if isinstance(obs_shape, int):
            self.obs_shape = (obs_shape,)
        else:
            self.obs_shape = tuple(obs_shape)
        self.n_actions = n_actions
        self.epsilon = epsilon_start
        self.epsilon_end = epsilon_end
        self.epsilon_decay = epsilon_decay
        self.batch_size = batch_size
        self.device = torch.device(device)

        self.policy_net = PolicyNetwork(self.obs_shape, n_actions, hidden).to(self.device)
        self.optimizer = optim.Adam(self.policy_net.parameters(), lr=lr)
        self.loss_fn = nn.CrossEntropyLoss()

        self.expert_buffer = ExpertBuffer(expert_buffer_capacity)
        self.episodes_done = 0
        self.train_steps = 0

    def select_action(self, state: np.ndarray, greedy: bool = False) -> int:
        """Epsilon-greedy action selection."""
        if not greedy and random.random() < self.epsilon:
            return random.randrange(self.n_actions)
        with torch.no_grad():
            s = torch.tensor(state, dtype=torch.float32, device=self.device).unsqueeze(0)
            logits = self.policy_net(s)
            return int(logits.argmax(dim=1).item())

    def store_expert_label(self, state, action):
        """Store an oracle (state, action) label in the expert buffer."""
        self.expert_buffer.push(state, action)

    def train_step(self, batch_size: int | None = None) -> float | None:
        """
        Sample a minibatch from the expert buffer and do one BC gradient step.
        Returns the loss value, or None if buffer too small.
        """
        bs = batch_size or self.batch_size
        if len(self.expert_buffer) < bs:
            return None

        states, actions = self.expert_buffer.sample(bs)
        states_t = torch.tensor(states, dtype=torch.float32, device=self.device)
        actions_t = torch.tensor(actions, dtype=torch.long, device=self.device)

        logits = self.policy_net(states_t)
        loss = self.loss_fn(logits, actions_t)

        self.optimizer.zero_grad()
        loss.backward()
        torch.nn.utils.clip_grad_norm_(self.policy_net.parameters(), max_norm=10.0)
        self.optimizer.step()

        self.train_steps += 1
        return loss.item()

    def end_episode(self):
        """Call at the end of each episode to decay epsilon."""
        self.episodes_done += 1
        self.epsilon = max(self.epsilon_end, self.epsilon * self.epsilon_decay)

    def save(self, path: str):
        """Save model weights."""
        torch.save({
            "policy_net": self.policy_net.state_dict(),
            "optimizer": self.optimizer.state_dict(),
            "epsilon": self.epsilon,
            "episodes_done": self.episodes_done,
            "train_steps": self.train_steps,
            "obs_shape": self.obs_shape,
        }, path)

    def load(self, path: str):
        """Load model weights."""
        ckpt = torch.load(path, map_location=self.device, weights_only=True)
        self.policy_net.load_state_dict(ckpt["policy_net"])
        if "optimizer" in ckpt:
            self.optimizer.load_state_dict(ckpt["optimizer"])
        self.epsilon = ckpt.get("epsilon", self.epsilon)
        self.episodes_done = ckpt.get("episodes_done", self.episodes_done)
        self.train_steps = ckpt.get("train_steps", self.train_steps)
