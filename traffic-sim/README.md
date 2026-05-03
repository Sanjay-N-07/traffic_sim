# Dynamic Traffic Flow & Congestion Simulation System
## Full-Stack Project — C Backend + Node.js Bridge + HTML/CSS/JS Frontend

---

## Folder Structure

```
traffic-sim/
├── backend/
│   ├── traffic_sim.c        ← C simulation (modified to output JSON)
│   ├── traffic_sim          ← compiled C binary (run npm run build-c)
│   └── server.js            ← Node.js HTTP bridge server
├── frontend/
│   ├── index.html           ← main UI page
│   ├── styles.css           ← dark dashboard stylesheet
│   └── script.js            ← all frontend logic (fetch, render, step)
├── package.json
└── README.md
```

---

## How the System Works

```
Browser (HTML/CSS/JS)
      |
      |  POST /simulate  { location:1, timesteps:15 }
      |  (fetch API)
      ▼
Node.js Server  (server.js — port 3001)
      |
      |  spawn child process
      |  write config JSON to stdin
      ▼
C Binary  (traffic_sim)
      |
      |  runs full simulation
      |  outputs ONE JSON blob to stdout
      ▼
Node.js reads stdout, parses JSON, sends to browser
      |
      ▼
Browser renders:
  - Road congestion bars (per step)
  - Intersection signal lights
  - Event log (per step)
  - Heat map
  - Road stats table
  - Vehicle log
  - Final report
```

---

## Setup Instructions

### Step 1 — Compile the C Binary

```bash
cd traffic-sim
npm run build-c
# or manually:
gcc -Wall -O2 -o backend/traffic_sim backend/traffic_sim.c
```

### Step 2 — Install Node.js Dependencies

```bash
npm install
```

### Step 3 — Start the Server

```bash
npm start
# or:
node backend/server.js
```

### Step 4 — Open the Browser

```
http://localhost:3001/index.html
```

---

## What Each File Does

| File | Purpose |
|------|---------|
| `traffic_sim.c` | Full C simulation. Reads JSON config from stdin. Outputs all results as one JSON object to stdout. Includes locations: Thiruvanmiyur, Adyar, Velachery |
| `server.js` | Express HTTP server. Receives POST /simulate from browser, spawns C binary, pipes config to stdin, reads JSON from stdout, returns to browser |
| `index.html` | Dashboard layout: controls panel, visual centre, analytics right panel with tabs |
| `styles.css` | Dark theme dashboard. Road bars, signal lamps, heat map, tables, badges |
| `script.js` | Calls API, renders step-by-step animation, heat map, vehicle log, final report |

---

## API Reference

### POST /simulate
**Request body:**
```json
{
  "location": 1,
  "timesteps": 15
}
```
- `location`: 1 = Thiruvanmiyur, 2 = Adyar, 3 = Velachery

**Response:**
```json
{
  "location": "Thiruvanmiyur Signal, Chennai",
  "summary": { "total_vehicles": 25, "completed": 20, "avg_wait": 3.5, ... },
  "roads": [ { "id":1, "name":"OMR Northbound", "congestion":44, ... }, ... ],
  "intersections": [ { "id":1, "name":"...", "cycle":9, ... }, ... ],
  "vehicles": [ { "id":1, "type":"NORMAL", "status":"DONE", "wait":2, ... }, ... ],
  "steps": [ { "step":0, "roads":[...], "intersections":[...], "events":"..." }, ... ],
  "heatmap": [44, 12, 0, ...]
}
```

### GET /health
Returns `{ "status": "ok" }` — use to verify server is running.

---

## Features

- **3 real Chennai locations** — Thiruvanmiyur, Adyar, Velachery
- **Step-by-step playback** — navigate each timestep manually
- **Auto-play** — watch traffic flow at 600ms per step
- **Road congestion bars** — colour-coded green → yellow → orange → red
- **Signal lamps** — shows which road has green at each intersection
- **Event log** — arrivals, emergencies, congestion, spillback per step
- **Congestion heat map** — final state of all roads
- **Road stats table** — max queue, congestion index, delay
- **Vehicle log** — type, source, destination, wait time, status
- **Final report** — completion rate, avg wait, avg travel, incidents

---

## Suggested Improvements

1. **WebSocket** — replace HTTP polling with WebSocket so the C backend can push each step in real time instead of returning all steps at once
2. **Canvas animation** — draw an actual road map with moving vehicle dots using HTML5 Canvas
3. **Custom input mode** — expose the full custom network builder from the C code through the UI
4. **Charts** — add a line chart of congestion over time per road using Chart.js
5. **Export** — add a button to download the final JSON report
