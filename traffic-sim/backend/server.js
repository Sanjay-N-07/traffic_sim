/*
  server.js — Node.js Bridge Server
  Sits between the browser and the C simulation binary.
  How it works:
  Browser --POST /simulate--> Node.js --spawn--> C binary --JSON--> Node.js --JSON--> Browser
 
  Install: npm install express cors
  Run:     node server.js
 */

const express   = require('express');
const cors      = require('cors');
const { spawn } = require('child_process');
const path      = require('path');

const app  = express();
const PORT = 3001;

app.use(cors());
app.use(express.json());
app.use(express.static(path.join(__dirname, '../frontend')));

/*
 * POST /simulate
 * Body: { location: 1|2|3, timesteps: 15, vehicles: 20 }
 * Spawns the C binary, sends config via stdin, reads JSON from stdout.
 */
app.post('/simulate', (req, res) => {
    const config = {
        location : req.body.location  || 1,
        timesteps: req.body.timesteps || 15,
        vehicles : req.body.vehicles  || 0,   /* 0 = use location default */
    };

    const binaryPath = path.join(__dirname, 'traffic_sim');
    const child      = spawn(binaryPath);

    let output = '';
    let errOut = '';

    child.stdin.write(JSON.stringify(config));
    child.stdin.end();

    child.stdout.on('data', (data) => { output += data.toString(); });
    child.stderr.on('data', (data) => { errOut += data.toString(); });

    child.on('close', (code) => {
        if (code !== 0) {
            console.error('C binary error:', errOut);
            return res.status(500).json({ error: 'Simulation failed', details: errOut });
        }
        try {
            const result = JSON.parse(output);
            res.json(result);
        } catch (e) {
            console.error('JSON parse error:', e.message);
            console.error('Raw output:', output.slice(0, 500));
            res.status(500).json({ error: 'Invalid JSON from C binary' });
        }
    });

    child.on('error', (err) => {
        console.error('Failed to spawn C binary:', err.message);
        res.status(500).json({ error: 'Could not start simulation binary' });
    });
});

app.get('/health', (req, res) => {
    res.json({ status: 'ok', message: 'Traffic Simulation Server is running' });
});

app.listen(PORT, () => {
    console.log(`\n  Traffic Simulation Server`);
    console.log(`  Running at http://localhost:${PORT}`);
    console.log(`  Open your browser at http://localhost:${PORT}/index.html\n`);
});
