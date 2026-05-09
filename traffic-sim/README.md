Dynamic Traffic Flow & Congestion Simulation System

A full-stack traffic simulation project using:

C → Traffic simulation backend
Node.js → Backend bridge/server
HTML, CSS, JavaScript → Frontend dashboard
Project Structure
traffic-sim/
│
├── backend/
│   ├── traffic_sim.c
│   ├── traffic_sim
│   └── server.js
│
├── frontend/
│   ├── index.html
│   ├── styles.css
│   └── script.js
│
├── package.json
└── README.md
How the System Works
Frontend (HTML/CSS/JS)
        ↓
Node.js Server
        ↓
C Simulation Engine
        ↓
JSON Output
        ↓
Frontend Visualization

Frontend sends simulation input
Node.js runs the C program
C performs traffic simulation
Results are sent back as JSON

Frontend displays congestion, signals, vehicles, and analytics

Setup Instructions
1. Compile the C Program
gcc -Wall -O2 -o backend/traffic_sim backend/traffic_sim.c
2. Install Dependencies
npm install
3. Start the Server
node backend/server.js
4. Open in Browser
http://localhost:3001/index.html

Features:
Adaptive traffic signal control
Congestion simulation
Emergency vehicle priority
Deadlock detection and recovery
Spillback detection
Step-by-step simulation
Auto-play visualization
Congestion heatmap
Vehicle analytics
Real Chennai road topologies

Locations Included
Thiruvanmiyur
Adyar
Velachery

Technologies Used
Technology	Purpose
C	Traffic simulation
Node.js	Server and integration
HTML	Webpage structure
CSS	Styling
JavaScript	Frontend logic
JSON	Data communication
POST /simulate
Example Input
{
  "location": 1,
  "timesteps": 15
}
GET /health

Checks whether the server is running.

Future Improvements:
Real-time WebSocket updates
Live vehicle animation
AI-based signal optimization
Real GPS traffic integration
Export simulation reports