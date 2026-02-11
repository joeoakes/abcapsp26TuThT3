# Vector – Maze Mission Schema (Team 3)

This document defines the refined Maze Mission data schema used by Team 3.

The schema is designed to support:

- Real-time telemetry logging (MongoDB)
- Redis for real-time retrieval and decision support
- Redis Vector for AI-generated embeddings, enabling fast semantic search and retrieval
- Mission reconstruction and replay

The structure aligns with the system architecture:

GameHat / Robot → Logger Server → AI Server

---

# 1. Mission Session Schema

A Mission Session represents one complete maze run.

## Primary Key

- `session_id`

## Fields

- `session_id` (string UUID)
- `robot_id` (string)
- `start_time` (ISO-8601 string)
- `end_time` (ISO-8601 string)
- `mission_result` (success | failed | aborted)
- `moves_total` (integer)

---

# 2. Mission Event Schema

Each movement during a session generates one Mission Event.

## Primary Key

- `mission_event_id`

## Foreign Key

- `session_id` → references Mission Session

## Fields

- `mission_event_id` (string UUID)
- `session_id` (string UUID)
- `input` (object)
  - `device` (string)

- `moves_left` (integer)
- `moves_right` (integer)
- `moves_straight` (integer)
- `moves_reverse` (integer)

- `distance_traveled` (integer)
- `mission_result` (success | failed | aborted)

---

# 3. Data Relationships

- One Mission Session → Many Mission Events
- All Mission Events with the same `session_id` belong to the same mission run
- `session_id` acts as a foreign key in Mission Event
