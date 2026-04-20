# Imitation Learning Maze Navigation

---

## Setup

Initialize a venv and install pytorch and numpy.

Use CUDA if you can: `pip3 install torch torchvision --index-url https://download.pytorch.org/whl/cu128`

Run the makefile to compile the maze and library (from `maze` dir): `make`

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

---

## Remote ML Server (Spark inference)

The maze window can hand control to a trained ML checkpoint running on the AI server.

### Spark Server

```bash
# On your local machine, in https_final/certs:
SPARK_IP=10.170.8.109

# 1. New key + CSR for the Spark
openssl genrsa -out spark_server.key 4096
openssl req -new -key spark_server.key -subj "/CN=dgx-spark" -out spark_server.csr

# 2. Sign it with the existing CA, binding the Spark's IP into the SAN
openssl x509 -req -in spark_server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out spark_server.crt -days 825 -sha256 -extfile <(printf "subjectAltName=DNS:dgx-spark,IP:%s" "$SPARK_IP")

# 3. Verify the SAN made it in
openssl x509 -in spark_server.crt -noout -subject -issuer -ext subjectAltName

# 4. Copy to Spark
scp spark_server.crt spark_server.key ca.crt YOUR_USER_ID@10.170.8.109:~/abcapsp26TuThT3/maze/certs/
```

```bash
# On the Spark
cd /path/to/abcapsp26TuThT3/maze
mkdir -p certs

# Start the inference server
python3 policy_server.py
```

Optional env vars:

| Variable            | Default                      | Description                          |
|---------------------|------------------------------|--------------------------------------|
| `POLICY_PORT`       | `8445`                       | HTTPS listen port                    |
| `POLICY_CHECKPOINT` | `checkpoints/dagger_best.pt` | Trained .pt checkpoint               |
| `POLICY_DEVICE`     | `cuda` if available          | `cpu` or `cuda`                      |
| `POLICY_HIDDEN`     | `256`                        | Must match training                  |
| `CERT_FILE`         | `certs/server.crt`           | TLS server cert                      |
| `KEY_FILE`          | `certs/server.key`           | TLS server key                       |
| `CA_CERT_FILE`      | `certs/ca.crt`               | CA used to verify clients            |
| `POLICY_VERIFY_PEER`| `1`                          | Set to `0` to fall back to plain TLS |

Peer verification is **on** by default. The server requires a valid client
cert signed by `CA_CERT_FILE` and the maze client must be launched with
`POLICY_CLIENT_CERT`, `POLICY_CLIENT_KEY`, and `POLICY_CA_CERT` set. Set
`POLICY_VERIFY_PEER=0` on both sides if you need to temporarily smoke-test
the wiring without distributing certs.

### Maze Client

Build and run `maze_sdl2` as usual, then press **N** in the maze window to toggle remote auto-nav. The client will:

1. Snap the agent back to `(0,0)` if it's not already there and clear the visited map.
2. Every `AUTO_NAV_DELAY` ms: POST the full maze state (walls + visited + agent + goal) to `POLICY_URL` and apply the returned action.
3. Give up after 1000 policy steps if the goal hasn't been reached.

```bash
cd maze
AUTO_NAV_DELAY=1 ./maze_sdl2
```

To temporarily disable mTLS on both ends (e.g., while debugging):

```bash
# Server
POLICY_VERIFY_PEER=0 python3 policy_server.py
# Client
POLICY_VERIFY_PEER=0 POLICY_URL="https://10.170.8.109:8445/policy" ./maze_sdl2
```