import random

import numpy as np
import pytest

torch = pytest.importorskip("torch")

from dagger import agent as dagger_agent


@pytest.fixture(autouse=True)
def deterministic_seeds():
    random.seed(1234)
    np.random.seed(1234)
    torch.manual_seed(1234)


# B2-35
def test_policy_network_vector_input_produces_expected_logits_shape():
    network = dagger_agent.PolicyNetwork((6,), n_actions=4, hidden=32)
    batch = torch.zeros(3, 6)

    logits = network(batch)

    assert logits.shape == (3, 4)


# B2-36
def test_policy_network_spatial_input_produces_expected_logits_shape():
    network = dagger_agent.PolicyNetwork((7, 3, 3), n_actions=4, hidden=32)
    batch = torch.zeros(2, 7, 3, 3)

    logits = network(batch)

    assert logits.shape == (2, 4)


# B2-37
def test_policy_network_accepts_unbatched_vector_input():
    network = dagger_agent.PolicyNetwork((5,), n_actions=3, hidden=16)
    state = torch.zeros(5)

    logits = network(state)

    assert logits.shape == (1, 3)


# B2-38
def test_policy_network_rejects_invalid_observation_shape():
    with pytest.raises(ValueError):
        dagger_agent.PolicyNetwork((2, 2), n_actions=4)


# B2-39
def test_expert_buffer_push_sample_and_length(monkeypatch):
    buffer = dagger_agent.ExpertBuffer(capacity=3)
    states = [
        np.array([1.0, 0.0], dtype=np.float32),
        np.array([0.0, 1.0], dtype=np.float32),
    ]
    buffer.push(states[0], 1)
    buffer.push(states[1], 2)
    monkeypatch.setattr(dagger_agent.random, "sample", lambda seq, n: list(seq)[:n])

    sampled_states, sampled_actions = buffer.sample(2)

    assert len(buffer) == 2
    assert sampled_states.shape == (2, 2)
    assert sampled_states.dtype == np.float32
    assert sampled_actions.shape == (2,)
    assert sampled_actions.dtype == np.int64
    assert np.array_equal(sampled_states[0], states[0])
    assert np.array_equal(sampled_actions, np.array([1, 2], dtype=np.int64))


# B2-40
def test_expert_buffer_respects_capacity():
    buffer = dagger_agent.ExpertBuffer(capacity=2)
    buffer.push(np.array([1.0], dtype=np.float32), 0)
    buffer.push(np.array([2.0], dtype=np.float32), 1)
    buffer.push(np.array([3.0], dtype=np.float32), 2)

    assert len(buffer) == 2
    assert [sample.action for sample in buffer.buffer] == [1, 2]


# B2-41
def test_dagger_agent_normalizes_integer_obs_shape():
    agent = dagger_agent.DAggerAgent(obs_shape=6, n_actions=4, hidden=16)

    assert agent.obs_shape == (6,)


# B2-42
def test_select_action_greedy_uses_policy_prediction(monkeypatch):
    agent = dagger_agent.DAggerAgent(obs_shape=4, n_actions=3, hidden=16)
    monkeypatch.setattr(
        agent.policy_net,
        "forward",
        lambda x: torch.tensor([[0.1, 1.2, -0.4]], dtype=torch.float32, device=agent.device),
    )

    action = agent.select_action(np.zeros(4, dtype=np.float32), greedy=True)

    assert action == 1


# B2-43
def test_select_action_uses_exploration_when_random_below_epsilon(monkeypatch):
    agent = dagger_agent.DAggerAgent(obs_shape=4, n_actions=4, hidden=16, epsilon_start=0.9)
    monkeypatch.setattr(dagger_agent.random, "random", lambda: 0.1)
    monkeypatch.setattr(dagger_agent.random, "randrange", lambda n: 3)

    action = agent.select_action(np.zeros(4, dtype=np.float32), greedy=False)

    assert action == 3


# B2-44
def test_store_expert_label_appends_to_buffer():
    agent = dagger_agent.DAggerAgent(obs_shape=4, n_actions=4, hidden=16)

    agent.store_expert_label(np.ones(4, dtype=np.float32), 2)

    assert len(agent.expert_buffer) == 1
    assert agent.expert_buffer.buffer[0].action == 2


# B2-45
def test_train_step_returns_none_when_buffer_too_small():
    agent = dagger_agent.DAggerAgent(obs_shape=4, n_actions=3, hidden=16, batch_size=4)
    agent.store_expert_label(np.zeros(4, dtype=np.float32), 0)

    loss = agent.train_step()

    assert loss is None
    assert agent.train_steps == 0


# B2-46
def test_train_step_returns_loss_and_increments_counter(monkeypatch):
    agent = dagger_agent.DAggerAgent(obs_shape=4, n_actions=3, hidden=16, batch_size=2)
    agent.store_expert_label(np.array([1.0, 0.0, 0.0, 0.0], dtype=np.float32), 0)
    agent.store_expert_label(np.array([0.0, 1.0, 0.0, 0.0], dtype=np.float32), 1)
    monkeypatch.setattr(dagger_agent.random, "sample", lambda seq, n: list(seq)[:n])

    loss = agent.train_step()

    assert isinstance(loss, float)
    assert loss >= 0.0
    assert agent.train_steps == 1


# B2-47
def test_end_episode_decays_epsilon_but_not_below_minimum():
    agent = dagger_agent.DAggerAgent(
        obs_shape=4,
        n_actions=3,
        hidden=16,
        epsilon_start=1.0,
        epsilon_end=0.2,
        epsilon_decay=0.5,
    )

    agent.end_episode()
    assert agent.epsilon == pytest.approx(0.5)

    agent.end_episode()
    assert agent.epsilon == pytest.approx(0.25)

    agent.end_episode()
    assert agent.epsilon == pytest.approx(0.2)
    assert agent.episodes_done == 3


# B2-48
def test_save_and_load_round_trip(tmp_path):
    source = dagger_agent.DAggerAgent(obs_shape=4, n_actions=3, hidden=16)
    source.epsilon = 0.37
    source.episodes_done = 5
    source.train_steps = 7

    with torch.no_grad():
        for parameter in source.policy_net.parameters():
            parameter.add_(0.25)

    checkpoint_path = tmp_path / "dagger_agent.pt"
    source.save(str(checkpoint_path))

    restored = dagger_agent.DAggerAgent(obs_shape=4, n_actions=3, hidden=16)
    restored.load(str(checkpoint_path))

    assert restored.epsilon == pytest.approx(0.37)
    assert restored.episodes_done == 5
    assert restored.train_steps == 7

    source_state = source.policy_net.state_dict()
    restored_state = restored.policy_net.state_dict()
    assert source_state.keys() == restored_state.keys()
    for name in source_state:
        assert torch.equal(source_state[name], restored_state[name])
