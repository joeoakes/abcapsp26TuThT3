"""Additional unit tests for DAggerAgent (B3-10)."""

import numpy as np
import pytest

torch = pytest.importorskip("torch")

from dagger import agent as dagger_agent


# B3-10
def test_policy_network_forward_is_deterministic_under_no_grad():
    """Two forward passes with identical input produce identical logits."""
    torch.manual_seed(999)
    net = dagger_agent.PolicyNetwork((7, 3, 3), n_actions=4, hidden=32)
    net.eval()

    state = torch.randn(1, 7, 3, 3)

    with torch.no_grad():
        logits1 = net(state)
        logits2 = net(state)

    assert torch.equal(logits1, logits2)
