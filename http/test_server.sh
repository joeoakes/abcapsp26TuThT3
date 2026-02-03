#!/bin/bash
# Test the maze_http_mongo server with sample data

echo "Sending test data to server..."
curl -s -X POST http://localhost:8080/move \
  -H "Content-Type: application/json" \
  -d '{
    "event_type": "player_move",
    "input": {
      "device": "joystick",
      "move_sequence": 1
    },
    "player": {
      "position": {
        "x": 1,
        "y": 2
      }
    },
    "goal_reached": false,
    "timestamp": "2026-01-28T12:00:00Z"
  }'
echo ""
echo "Done!"
