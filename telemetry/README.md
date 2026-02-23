## Overview 

This project implements a telemetry pipeline for the Maze SDL2 application. 

## System Components

## Client: `maze/maze_sdl2.c` 

- Generates JSON telemetry for player move events 

- Generates mission summary payloads 

- Sends data via HTTPS using libcurl 

---

## Logging Server: `https/maze_https_mongo.c` 

- HTTPS server with mTLS 

- Port: 8445 

- Receives telemetry from maze client at `POST /move` 

- Stores telemetry in MongoDB 

 

## Mini-Pupper Server: `robot/robot_bridge.py` 

- HTTPS server with mTLS 

- Port: 8445 

- Receives telemetry from maze client at `POST /robot` 

- Executes corresponding action with A Twist command 

- Publishes the Twist command to `/cmd_vel` to move the Mini-Pupper 

 

## Telemetry payload 

 

  { 

    "event_type": "player_move", 

    "input": { 

      "device": "keyboard", 

      "move_sequence": 1 

    }, 

    "player": { 

      "position": { 

        "x": 1, 

        "y": 0 

      } 

    }, 

    "goal_reached": false, 

    "timestamp": "2026-02-17T03:30:00Z", 

    "action": "forward" // Used by Mini-Pupper 

  } 

 

Example Mini-Pupper CURL command (move forward 1 unit): 

`curl -k -X POST https://localhost:8445/robot -H "Content-Type: application/json" -d '{"action":"forward"}'` 

  

  Mission payload: 

  { 

    "mission_id": "DEMO_MISSION", 

    "robot_id": "ROBOT_01", 

    "mission_type": "patrol", 

    "start_time": 1770056813, 

    "end_time": 1770056848, 

    "moves_left_turn": 46, 

    "moves_right_turn": 46, 

    "moves_straight": 52, 

    "moves_reverse": 8, 

    "moves_total": 152, 

    "distance_traveled": 24.41, 

    "duration_seconds": 35, 

    "mission_result": "success", 

    "abort_reason": "user exited" 

  } 


