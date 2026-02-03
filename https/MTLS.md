# How mTLS Authentication Works

## The Short Version

Regular HTTPS proves the **server** is who it says it is.
mTLS (mutual TLS) goes one step further — it also proves the **client** is who it says it is.
Both sides verify each other before any data is sent.

---

## The Analogy: The Secure Building

Imagine you need to get into a secure building at a company.

### Step 1 — You verify the building is real

When you walk up, the building has a **badge on the door** that proves it is the real company building. That badge was stamped by a **trusted authority** (think: the city government issued the building permit). You check the badge, trust the stamp, and know you are at the right place.

This is what normal HTTPS does. Your browser checks the server's certificate. The certificate was signed by a trusted authority (a Certificate Authority). If the stamp is valid, you know you are talking to the real server.

### Step 2 — The building verifies you

In a normal building, anyone can walk in after checking the badge. But in a **secure** building, the door also checks **your ID**.

Your ID card was stamped by the **same trusted authority**. The door scanner reads your card, checks the stamp, and confirms you are an authorized person before letting you in.

This is the "mutual" part of mTLS. The server checks your client certificate the same way you checked the server's certificate. Both sides trust the same authority.

### The authority is the key

Neither the building badge nor your ID card are useful on their own. They only work because they were both stamped by the same trusted authority. If someone fakes a badge or a fake ID that was not stamped by that authority, it gets rejected.

---

## How It Maps to Our Maze Project

| Analogy | What It Actually Is | File in the Project |
|---|---|---|
| The trusted authority | Certificate Authority (CA) | `certs/ca.crt` + `certs/ca.key` |
| The authority's stamp | The CA's private key signs the certs | `certs/ca.key` |
| The building's badge | Server certificate | `certs/server.crt` |
| Your ID card | Client certificate | `certs/client.crt` |
| The door scanner | Server checks client cert at connection time | `maze_https_mongo.c` |
| You checking the badge | Maze app verifies server cert before connecting | `maze_sdl2.c` |

### The CA (Certificate Authority)

`ca.key` is the most important file. It is the private key that was used to **sign** both the server and client certificates. Anyone who has `ca.key` can create new certificates that the server will trust. Anyone who does not have it cannot.

`ca.crt` is the public side — it is what the server and client use to **verify** that a certificate was signed by the CA. Both the server and the maze app have a copy of this file.

### The Server Certificate

`server.crt` is the server's identity. It says "I am `localhost`" and it is signed by `MazeLab-CA`. When the maze app connects, it checks this certificate against `ca.crt` to confirm it is really talking to the right server.

### The Client Certificate

`client.crt` is the maze app's identity. It says "I am `maze-client`" and it is also signed by `MazeLab-CA`. When the server sees a connection, it checks this certificate against `ca.crt` to confirm the request is coming from an authorized client.

---

## What Happens Step by Step When the Maze App Sends a Move

1. The maze app initiates a connection to `https://localhost:8443/move`.
2. The server sends its certificate (`server.crt`) to the maze app.
3. The maze app checks: was this certificate signed by `MazeLab-CA`? If yes, continue. If no, abort.
4. The server requests a certificate from the maze app.
5. The maze app sends its certificate (`client.crt`).
6. The server checks: was this certificate signed by `MazeLab-CA`? If yes, continue. If no, reject the connection.
7. Both sides have verified each other. The connection is encrypted and trusted.
8. The maze app sends the JSON telemetry (player position, move number, etc.).
9. The server inserts it into MongoDB and responds with `{"status":"ok"}`.

---

## What the Server Actually Checks

The server accepts a client certificate if it meets these conditions:

- It was signed by `MazeLab-CA` (verified by GnuTLS automatically)
- It is not expired (valid until Feb 2027)
- A certificate was actually presented (checked in the server code)

The server does **not** currently check the client's CN (name), IP address, or any other details. Any valid certificate signed by `MazeLab-CA` is accepted.
