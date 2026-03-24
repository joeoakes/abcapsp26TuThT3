# 🐾 Team 3 – Mini-Pupper Mission Dashboard (Mockup)

## Overview

This folder contains the mockup design for the Team 3 Mini-Pupper Mission Dashboard.

The dashboard represents the planned frontend interface that will visualize:

- Mission status  
- Telemetry data  
- Robot movement information  
- AI and logging system health  

⚠️ This is currently a static mockup (no live backend integration).

---

## Purpose

The mission dashboard is designed to:

- Display mission status
- Show robot connection state
- Visualize telemetry metrics
- Present movement and position data
- Indicate MongoDB & AI server health
- Reflect secure communication status (HTTPS / mTLS)

---

## System Architecture Context

Telemetry flow in our system:

GameHat Maze App  
↓ HTTPS (Port 8445)  
Mini-Pupper Telemetry Receiver  
↓  
Logging Server (MongoDB)  
↓  
AI Server  

The dashboard is intended to visualize processed telemetry and AI outputs from this pipeline.

---

## Dashboard Sections

### 1.Status Overview

- Mission Status  
- Robot Status (Connected)  
- Total Telemetry Events  
- Mission Timer (Session Runtime)  
- Secure Connection Indicator (HTTPS / mTLS)

### 2.Telemetry Summary

- Total Commands  
- Successful Moves  
- Failed Commands  
- Success Rate  
- Last Event Timestamp  

### 3.Movement & Position

- Current (X, Y) Maze Coordinates  
- Direction  
- Speed  
- Goal Reached Indicator  
- Maze Map Visualization Placeholder  

### 4.AI & Logging

- MongoDB Status  
- AI Server Status  
- Secure Channel Status  
- mTLS Status  
- Last Model Output  

---

## Security Representation

The dashboard reflects the system's secure telemetry design:

- HTTPS is used for telemetry transmission  
- mTLS support is planned / enforced  
- Secure connection status is displayed in the UI  

---

## Current Status

This is a visual mockup only.

- No live database connection  
- No real-time telemetry feed  
- Static sample data used for layout demonstration  

---

## Future Enhancements

Planned improvements include:

- Live API integration with backend telemetry service  
- WebSocket-based real-time updates  
- Real-time maze visualization  
- Command history timeline  
- Error analytics charts  
- Authentication & role-based access control  

---

## Team 3 – CMPSC Project

Mini-Pupper + Maze App + Telemetry + AI Integration

--- 

## Current Status

![Mission Dashboard Preview](dashboard.png)
