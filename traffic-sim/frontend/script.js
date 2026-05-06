/*
 * script.js — Traffic Simulation Frontend Logic (Fixed)
 *
 * Changes from original:
 *  1. Renders road_cong from step snapshots (raw, not EMA-lagged) — congestion
 *     now shows 0% correctly when all vehicles have completed.
 *  2. Active vehicle count badge shown in step label so users see it drop to 0.
 *  3. Vehicle count slider + number input wired to runSimulation() payload.
 *  4. Congestion bar width driven by raw count-based % for accurate live view.
 */

const API_BASE = 'http://localhost:3001';

let simData       = null;
let currentStep   = 0;
let autoPlayTimer = null;
let selectedLoc   = 1;
let vehicleCount  = 0;   /* 0 = use location default */

/* ---- Location card selection ---- */
document.querySelectorAll('.loc-card').forEach(card => {
    card.addEventListener('click', () => {
        document.querySelectorAll('.loc-card').forEach(c => c.classList.remove('active'));
        card.classList.add('active');
        selectedLoc = parseInt(card.dataset.loc);
        /* Redraw skeleton map for the new location if tab is open */
        updateNetworkMap();
    });
});

/* ---- Timestep slider ---- */
const tsSlider = document.getElementById('timestepSlider');
const tsHint   = document.getElementById('tsHint');
tsSlider.addEventListener('input', () => {
    const v = tsSlider.value;
    tsHint.textContent = `${v} steps (~${v * 2} min)`;
});

/* ---- Vehicle count controls ---- */
const vehicleSlider = document.getElementById('vehicleSlider');
const vehicleInput  = document.getElementById('vehicleInput');
const vehicleHint   = document.getElementById('vehicleHint');

function onVehicleSlider() {
    const v = parseInt(vehicleSlider.value);
    vehicleCount = v === 0 ? 0 : v;
    vehicleInput.value = v === 0 ? '' : v;
    vehicleHint.textContent = v === 0
        ? 'Default (location preset)'
        : `${v} vehicle${v !== 1 ? 's' : ''}`;
}

function onVehicleNumber() {
    const raw = parseInt(vehicleInput.value);
    if (isNaN(raw) || raw <= 0) {
        vehicleCount = 0;
        vehicleSlider.value = 0;
        vehicleHint.textContent = 'Default (location preset)';
    } else {
        vehicleCount = Math.min(raw, 300);
        vehicleSlider.value = Math.min(vehicleCount, 100);
        vehicleHint.textContent = `${vehicleCount} vehicle${vehicleCount !== 1 ? 's' : ''}`;
    }
}

/* ---- Status badge ---- */
function setStatus(state, text) {
    document.querySelector('.status-dot').className = 'status-dot ' + state;
    document.getElementById('statusText').textContent = text;
}

/* ---- Run simulation ---- */
async function runSimulation() {
    if (autoPlayTimer) { clearInterval(autoPlayTimer); autoPlayTimer = null; }

    document.getElementById('loadingOverlay').style.display = 'flex';
    document.getElementById('visualPlaceholder').style.display = 'none';
    document.getElementById('roadMap').style.display = 'none';
    document.getElementById('btnRun').disabled = true;
    setStatus('running', 'Simulating…');

    const config = {
        location : selectedLoc,
        timesteps: parseInt(tsSlider.value),
        vehicles : vehicleCount,   /* 0 = backend uses location default */
    };

    try {
        const response = await fetch(`${API_BASE}/simulate`, {
            method : 'POST',
            headers: { 'Content-Type': 'application/json' },
            body   : JSON.stringify(config)
        });

        if (!response.ok) {
            const err = await response.json();
            throw new Error(err.error || 'Server error');
        }

        simData = await response.json();
        currentStep = 0;

        renderStep(currentStep);
        renderHeatmap();
        renderRoadStats();
        renderVehicleLog();
        renderReport();
        renderQuickStats();

        document.getElementById('roadMap').style.display = 'block';
        document.getElementById('loadingOverlay').style.display = 'none';
        document.getElementById('stepControls').style.display = 'flex';
        document.getElementById('summaryCard').style.display  = 'flex';
        document.getElementById('btnStep').disabled = false;

        document.getElementById('curStep').textContent = currentStep;
        document.getElementById('maxStep').textContent = simData.steps.length - 1;

        setStatus('done', 'Simulation complete');

    } catch (err) {
        document.getElementById('loadingOverlay').style.display = 'none';
        document.getElementById('visualPlaceholder').style.display = 'flex';
        document.getElementById('visualPlaceholder').innerHTML =
            `<div class="placeholder-icon">⚠️</div>
             <p>Error: <strong>${err.message}</strong><br>
             Make sure the Node.js server is running on port 3001.</p>`;
        setStatus('error', 'Error');
        console.error('Simulation error:', err);
    }

    document.getElementById('btnRun').disabled = false;
}

/* ---- Render a single step ---- */
function renderStep(stepIdx) {
    if (!simData || !simData.steps || stepIdx >= simData.steps.length) return;

    const step = simData.steps[stepIdx];

    /* Step label — now includes active vehicle count */
    const timeMin = stepIdx * 2;
    const hh = 8 + Math.floor(timeMin / 60);
    const mm  = String(timeMin % 60).padStart(2, '0');
    const activeVeh = step.active_vehicles !== undefined ? step.active_vehicles : '?';
    document.getElementById('stepLabel').textContent =
        `Step ${String(stepIdx).padStart(2,'0')}  —  ${hh}:${mm} AM  |  🚗 ${activeVeh} active`;

    /* Road congestion bars — use raw road_cong (fixed: truly 0 when empty) */
    const roadBars = document.getElementById('roadBars');
    roadBars.innerHTML = '';

    step.roads.forEach((rd, i) => {
        const road = simData.roads[i];
        /* rd.cong is raw_congestion: 0 when no vehicles, honest % otherwise */
        const pct = rd.cong;
        const cls = pct >= 75 ? 'cong-crit' : pct >= 50 ? 'cong-high' : pct >= 25 ? 'cong-med' : 'cong-low';
        const col = pct >= 75 ? '#e74c3c'   : pct >= 50 ? '#e67e22'   : pct >= 25 ? '#f39c12'  : '#2ecc71';

        roadBars.innerHTML += `
            <div class="road-bar-row">
                <div class="road-bar-name" title="${road.name}">R${i+1} ${road.name}</div>
                <div class="road-bar-track">
                    <div class="road-bar-fill ${cls}"
                         style="width:${Math.max(pct, rd.count > 0 ? 4 : 0)}%;">
                    </div>
                </div>
                <div class="road-bar-pct" style="color:${col}">${pct}%</div>
            </div>`;
    });

    /* Intersection signals */
    const igrid = document.getElementById('intersectionGrid');
    igrid.innerHTML = '';

    step.intersections.forEach((inter, i) => {
        const intersection = simData.intersections[i];
        const greenRoad    = inter.green;
        const road         = simData.roads[greenRoad - 1];
        const roadName     = road ? road.name : `Road ${greenRoad}`;

        igrid.innerHTML += `
            <div class="inter-card">
                <div class="inter-name">I${i+1} — ${intersection.name}</div>
                <div class="inter-signal-row">
                    <div class="signal-lamp lamp-green"></div>
                    <div class="inter-road-label">R${greenRoad}</div>
                    <div class="inter-timer">${inter.timer}/${inter.adaptive}s</div>
                </div>
                <div style="font-size:11px;color:var(--text-hint);margin-top:4px;
                            white-space:nowrap;overflow:hidden;text-overflow:ellipsis;">
                    ${roadName}
                </div>
            </div>`;
    });

    /* Event log */
    const evContainer = document.getElementById('eventItems');
    evContainer.innerHTML = '';

    if (!step.events || step.events.trim() === '') {
        evContainer.innerHTML = '<div class="event-item">—</div>';
    } else {
        step.events.split('|').filter(e => e.trim()).forEach(ev => {
            const cls = ev.includes('ARRIVED') ? 'ev-arrive' :
                        ev.includes('EMERG')   ? 'ev-emerg'  :
                        ev.includes('CONG')    ? 'ev-cong'   :
                        ev.includes('SPILL')   ? 'ev-spill'  : 'ev-move';
            evContainer.innerHTML += `<div class="event-item ${cls}">${ev.trim()}</div>`;
        });
    }

    document.getElementById('curStep').textContent = stepIdx;

    /* Update network map if that tab is open */
    updateNetworkMap();
}

/* ---- Heat map ---- */
function renderHeatmap() {
    if (!simData) return;
    const wrap = document.getElementById('heatmapWrap');
    wrap.innerHTML = '';

    simData.roads.forEach((road, i) => {
        const cong = simData.heatmap[i] || 0;
        const bg   = congColor(cong);
        const fg   = cong > 50 ? '#fff' : '#111';

        wrap.innerHTML += `
            <div class="hm-row">
                <div class="hm-label">R${i+1}</div>
                <div class="hm-road-name" title="${road.name}">${road.name}</div>
                <div class="hm-cell" style="background:${bg};color:${fg}">
                    ${cong}%
                </div>
            </div>`;
    });
}

function congColor(pct) {
    if (pct >= 75) return '#c0392b';
    if (pct >= 50) return '#d35400';
    if (pct >= 25) return '#d4ac0d';
    return '#1e8449';
}

/* ---- Road stats table ---- */
function renderRoadStats() {
    if (!simData) return;

    let html = `
        <table class="stats-table">
            <thead>
                <tr><th>Road</th><th>Name</th><th>MaxQ</th><th>Congestion</th><th>Delay</th></tr>
            </thead>
            <tbody>`;

    simData.roads.forEach((r, i) => {
        const col = r.congestion >= 75 ? '#e74c3c' : r.congestion >= 50 ? '#e67e22' :
                    r.congestion >= 25 ? '#f39c12' : '#2ecc71';
        html += `
            <tr>
                <td class="road-id">R${i+1}</td>
                <td class="road-name" title="${r.name}">${r.name}</td>
                <td>${r.max_queue}</td>
                <td>
                    <div class="mini-bar">
                        <div class="mini-bar-track">
                            <div class="mini-bar-fill" style="width:${r.congestion}%;background:${col}"></div>
                        </div>
                        <span style="font-size:11px;color:${col}">${r.congestion}%</span>
                    </div>
                </td>
                <td>${r.delay}</td>
            </tr>`;
    });

    html += `</tbody></table>`;
    document.getElementById('roadStatsTable').innerHTML = html;
}

/* ---- Vehicle log ---- */
function renderVehicleLog() {
    if (!simData) return;

    let html = `
        <table class="veh-table">
            <thead>
                <tr><th>ID</th><th>Type</th><th>Src</th><th>Dst</th><th>Wait</th><th>Travel</th><th>Status</th></tr>
            </thead>
            <tbody>`;

    simData.vehicles.forEach(v => {
        const typeBadge   = v.type === 'EMERG'
            ? '<span class="badge badge-emerg">EMERG</span>'
            : '<span class="badge badge-normal">NORMAL</span>';
        const statusBadge = v.status === 'DONE'
            ? '<span class="badge badge-done">DONE</span>'
            : v.status === 'ACTIVE'
            ? '<span class="badge badge-active">ACTIVE</span>'
            : '<span class="badge badge-wait">WAIT</span>';

        html += `
            <tr>
                <td>V${v.id}</td><td>${typeBadge}</td>
                <td>R${v.src}</td><td>R${v.dst}</td>
                <td>${v.wait}</td><td>${v.travel}</td><td>${statusBadge}</td>
            </tr>`;
    });

    html += `</tbody></table>`;
    document.getElementById('vehicleLogTable').innerHTML = html;
}

/* ---- Final report ---- */
function renderReport() {
    if (!simData) return;
    const m    = simData.summary;
    const rate = m.total_vehicles > 0
        ? ((m.completed / m.total_vehicles) * 100).toFixed(1)
        : 0;
    const waitClass = m.avg_wait < 5 ? 'good' : m.avg_wait < 10 ? 'warn' : 'crit';

    let busiest = simData.roads[0];
    simData.roads.forEach(r => { if (r.max_queue > busiest.max_queue) busiest = r; });

    document.getElementById('finalReport').innerHTML = `
        <div class="report-section">
            <div class="report-heading">Simulation Summary</div>
            ${row('Location', simData.location)}
            ${row('Period', `08:00 — 08:${String(m.total_steps*2).padStart(2,'0')} AM`)}
            ${row('Total vehicles', m.total_vehicles)}
            ${row('Entered network', m.entered)}
            ${row('Completed journey', m.completed)}
            ${row('Completion rate', rate + '%', rate >= 80 ? 'good' : rate >= 50 ? 'warn' : 'crit')}
        </div>
        <div class="report-section">
            <div class="report-heading">Performance</div>
            ${row('Avg wait time', m.avg_wait.toFixed(2) + ' steps', waitClass)}
            ${row('Avg travel time', m.avg_travel.toFixed(2) + ' steps')}
            ${row('Max queue length', m.max_queue + ' vehicles')}
        </div>
        <div class="report-section">
            <div class="report-heading">Incidents</div>
            ${row('Emergency vehicles', m.emergency_dispatched)}
            ${row('Signal overrides', m.emergency_overrides)}
            ${row('Deadlocks', m.deadlocks, m.deadlocks === 0 ? 'good' : 'crit')}
            ${row('Spillback events', m.spillbacks, m.spillbacks === 0 ? 'good' : 'warn')}
        </div>
        <div class="report-section">
            <div class="report-heading">Peak-Hour Observation</div>
            ${row('Busiest road', `R${simData.roads.indexOf(busiest)+1} — ${busiest.name}`)}
            ${row('Peak queue there', busiest.max_queue + ' vehicles')}
        </div>`;
}

function row(key, val, cls = '') {
    return `<div class="report-row">
                <span class="report-key">${key}</span>
                <span class="report-val ${cls}">${val}</span>
            </div>`;
}

/* ---- Quick stats ---- */
function renderQuickStats() {
    if (!simData) return;
    const m = simData.summary;
    document.getElementById('sSummCompleted').textContent = m.completed;
    document.getElementById('sSummWait').textContent      = m.avg_wait.toFixed(1);
    document.getElementById('sSummEmerg').textContent     = m.emergency_dispatched;
    document.getElementById('sSummQueue').textContent     = m.max_queue;
}

/* ---- Step controls ---- */
function startStepMode() {
    if (!simData) return;
    currentStep = 0;
    renderStep(currentStep);
    document.getElementById('curStep').textContent = currentStep;
}

function prevStep() {
    if (!simData || currentStep <= 0) return;
    currentStep--;
    renderStep(currentStep);
}

function nextStep() {
    if (!simData || currentStep >= simData.steps.length - 1) return;
    currentStep++;
    renderStep(currentStep);
}

function autoPlay() {
    if (!simData) return;
    if (autoPlayTimer) {
        clearInterval(autoPlayTimer);
        autoPlayTimer = null;
        document.getElementById('btnPlay').textContent = '⏯ Auto Play';
        return;
    }
    document.getElementById('btnPlay').textContent = '⏹ Stop';
    autoPlayTimer = setInterval(() => {
        if (currentStep >= simData.steps.length - 1) {
            clearInterval(autoPlayTimer);
            autoPlayTimer = null;
            document.getElementById('btnPlay').textContent = '⏯ Auto Play';
            return;
        }
        currentStep++;
        renderStep(currentStep);
    }, 600);
}

/* ---- Tab switching ---- */
function switchTab(name, btn) {
    document.querySelectorAll('.tab-content').forEach(t => t.classList.remove('active'));
    document.querySelectorAll('.tab').forEach(b => b.classList.remove('active'));
    document.getElementById('tab-' + name).classList.add('active');
    btn.classList.add('active');
}

/* ---- Reset ---- */
function resetAll() {
    if (autoPlayTimer) { clearInterval(autoPlayTimer); autoPlayTimer = null; }
    simData = null; currentStep = 0;

    document.getElementById('roadMap').style.display = 'none';
    document.getElementById('visualPlaceholder').style.display = 'flex';
    document.getElementById('visualPlaceholder').innerHTML =
        `<div class="placeholder-icon">🗺️</div>
         <p>Select a location and click <strong>Run Simulation</strong></p>`;
    document.getElementById('stepControls').style.display = 'none';
    document.getElementById('summaryCard').style.display  = 'none';
    document.getElementById('btnStep').disabled = true;
    document.getElementById('heatmapWrap').innerHTML   = '<div class="no-data">No data yet — run the simulation first.</div>';
    document.getElementById('roadStatsTable').innerHTML = '<div class="no-data">No data yet.</div>';
    document.getElementById('vehicleLogTable').innerHTML = '<div class="no-data">No data yet.</div>';
    document.getElementById('finalReport').innerHTML    = '<div class="no-data">No data yet.</div>';
    setStatus('idle', 'Ready');
}

/* ============================================================
   PART 2 — ROAD NETWORK MAP
   Draws a static road graph (roads = edges, intersections = nodes)
   and colour-codes each road segment by its current congestion %.
   Updates live as the user steps through the simulation.
============================================================ */

/* ---- Per-location node + edge layout definitions ----
 * Coordinates are normalised 0..1 (scaled to canvas at draw time).
 * Each road entry carries:
 *   id  : 1-based road index (matches simData.roads[id-1])
 *   from: [x,y] start point  (or an intersection node id, e.g. "I0")
 *   to  : [x,y] end point
 * Intersections get explicit node positions.
 */
/*
 * =====================================================================
 *  ROAD NETWORK — Layout definitions + canvas renderer
 *
 *  Each edge carries:
 *    rid    : 1-based road id (matches simData.roads[rid-1])
 *    label  : display name "R1" … "R10"
 *    x1,y1  : start point  (normalised 0-1)
 *    x2,y2  : end point    (normalised 0-1)
 *    offset : signed pixel offset perpendicular to the road direction.
 *             Used to visually separate roads that share endpoints so
 *             they never merge into one line.  Positive = left of the
 *             direction vector, negative = right.
 *    labelSide : +1 (label left of road) | -1 (right) | 0 (auto)
 *
 *  Direction arrows reflect actual traffic flow:
 *    x1,y1 → x2,y2  is the direction traffic travels.
 *    "inbound to intersection" roads terminate at an intersection node.
 *    "outbound from intersection" roads originate at an intersection node.
 * =====================================================================
 */
const NETWORK_LAYOUTS = {

    /* ================================================================
       LOC 1 — THIRUVANMIYUR  (8 roads after adding R8)
       Verified backend topology:
         I1 (Main Signal) : in=[R1,R3,R4]       out=[R2,R5]
         I2 (RGS Merge)   : in=[R7]              out=[R4,R8]  ← R8 new
         I3 (Bus Stand)   : in=[R2,R5,R6,R8]   out=[]

       Node positions (normalised 0-1):
         I1 = (0.47, 0.35)  centre-upper
         I2 = (0.18, 0.72)  lower-left
         I3 = (0.78, 0.72)  lower-right

       Rule: every edge x2,y2 = its destination node's x,y
             (offset shifts the line sideways, never the endpoint)
    ================================================================ */
    1: {
        nodes: [
            { id:'I0', label:'I1\nMain Signal', x:0.47, y:0.35 },
            { id:'I1', label:'I2\nRGS Merge',   x:0.18, y:0.72 },
            { id:'I2', label:'I3\nBus Stand',   x:0.78, y:0.72 },
        ],
        edges: [
            /* R1  ext(top) → I1=(0.47,0.35)  ↓  endpoint pinned to I1 */
            { rid:1, label:'R1', x1:0.47, y1:0.08, x2:0.47, y2:0.35, offset:-7, labelSide:-1 },
            /* R2  I1=(0.47,0.35) → I3=(0.78,0.72)  ↘ UPPER diagonal — starts upper-left of I1 */
            { rid:2, label:'R2', x1:0.75, y1:0.69, x2:0.44, y2:0.32, offset:14, labelSide:1 },
            /* R3  ext(left) → I1=(0.47,0.35)  →  endpoint pinned */
            { rid:3, label:'R3', x1:0.08, y1:0.35, x2:0.47, y2:0.35, offset:-6, labelSide:-1 },
            /* R4  I2=(0.18,0.72) → I1=(0.47,0.35)  ↗  both pinned */
            { rid:4, label:'R4', x1:0.18, y1:0.72, x2:0.47, y2:0.35, offset: 6, labelSide: 1 },
            /* R5  I1=(0.47,0.35) → I3=(0.78,0.72)  ↘ LOWER diagonal — starts lower-right of I1 */
            { rid:5, label:'R5', x1:0.50, y1:0.38, x2:0.81, y2:0.75, offset: 14, labelSide: 1 },
            /* R6  ext(bottom-right) → I3=(0.78,0.72)  ↖  endpoint pinned */
            { rid:6, label:'R6', x1:0.94, y1:0.88, x2:0.78, y2:0.72, offset:-5, labelSide:-1 },
            /* R7  ext(bottom) → I2=(0.18,0.72)  ↑  endpoint pinned */
            { rid:7, label:'R7', x1:0.18, y1:0.94, x2:0.18, y2:0.72, offset:-6, labelSide:-1 },
            /* R8  I2=(0.18,0.72) → I3=(0.78,0.72)  →  both pinned */
            { rid:8, label:'R8', x1:0.18, y1:0.72, x2:0.78, y2:0.72, offset:-7, labelSide:-1 },
        ],
    },

    /* ================================================================
       LOC 2 — ADYAR  (9 roads)
       Verified backend topology:
         I1 (Adyar Main) : in=[R1,R3,R4]  out=[R2,R5]
         I2 (Depot Jn)   : in=[R2,R6,R7]  out=[]  terminus
         I3 (Bus Stand)  : in=[R5,R8]      out=[R9]

       Node positions:
         I1 = (0.38, 0.44)  centre-left
         I2 = (0.76, 0.28)  upper-right (depot)
         I3 = (0.38, 0.76)  lower-centre (bus stand)
    ================================================================ */
    2: {
        nodes: [
            { id:'I0', label:'I1\nAdyar Main', x:0.38, y:0.44 },
            { id:'I1', label:'I2\nDepot Jn',   x:0.76, y:0.28 },
            { id:'I2', label:'I3\nBus Stand',  x:0.38, y:0.76 },
        ],
        edges: [
            /* R1  ext(top) → I1=(0.38,0.44)  ↓  endpoint pinned to I1 */
            { rid:1, label:'R1', x1:0.38, y1:0.08, x2:0.38, y2:0.44, offset:-7, labelSide:-1 },
            /* R2  I1=(0.38,0.44) → I2=(0.76,0.28)  ↗  both pinned */
            { rid:2, label:'R2', x1:0.38, y1:0.44, x2:0.76, y2:0.28, offset:-6, labelSide:-1 },
            /* R3  ext(left) → I1=(0.38,0.44)  →  endpoint pinned */
            { rid:3, label:'R3', x1:0.08, y1:0.44, x2:0.38, y2:0.44, offset:-6, labelSide:-1 },
            /* R4  ext(upper-left) → I1=(0.38,0.44)  ↘  endpoint pinned */
            { rid:4, label:'R4', x1:0.10, y1:0.22, x2:0.38, y2:0.44, offset:-6, labelSide:-1 },
            /* R5  I1=(0.38,0.44) → I3=(0.38,0.76)  ↓  both pinned */
            { rid:5, label:'R5', x1:0.38, y1:0.44, x2:0.38, y2:0.76, offset: 7, labelSide: 1 },
            /* R6  I2=(0.76,0.28) → ext(upper-right)  ↗  arrow reversed to correct direction */
            { rid:6, label:'R6', x1:0.76, y1:0.28, x2:0.94, y2:0.12, offset:-5, labelSide:-1 },
            /* R7  ext(right) → I2=(0.76,0.28)  ↙  endpoint pinned */
            { rid:7, label:'R7', x1:0.94, y1:0.42, x2:0.76, y2:0.28, offset: 7, labelSide: 1 },
            /* R8  ext(lower-left) → I3=(0.38,0.76)  ↗  endpoint pinned */
            { rid:8, label:'R8', x1:0.08, y1:0.90, x2:0.38, y2:0.76, offset:-5, labelSide:-1 },
            /* R9  I3=(0.38,0.76) → ext(bottom)  ↓  start pinned */
            { rid:9, label:'R9', x1:0.38, y1:0.76, x2:0.38, y2:0.94, offset: 6, labelSide: 1 },
        ],
    },

    /* ================================================================
       LOC 3 — VELACHERY  (10 roads)
       Verified backend topology:
         I1 (Main Signal)  : in=[R1,R3,R4]    out=[R2,R5]
         I2 (Phoenix Mall) : in=[R2,R5,R6]    out=[R7,R10]
         I3 (South Jn)     : in=[R7,R8,R9]   out=[]  terminus

       Node positions:
         I1 = (0.45, 0.30)  upper-centre
         I2 = (0.45, 0.64)  lower-centre
         I3 = (0.78, 0.64)  lower-right
    ================================================================ */
    3: {
        nodes: [
            { id:'I0', label:'I1\nMain Signal',  x:0.45, y:0.30 },
            { id:'I1', label:'I2\nPhoenix Mall', x:0.45, y:0.64 },
            { id:'I2', label:'I3\nSouth Jn',     x:0.78, y:0.64 },
        ],
        edges: [
            /* R1   ext(top) → I1=(0.45,0.30)  ↓  endpoint pinned to I1 */
            { rid: 1, label:'R1',  x1:0.45, y1:0.08, x2:0.45, y2:0.30, offset:-8, labelSide:-1 },
            /* R2   I1=(0.45,0.30) → I2=(0.45,0.64)  ↓  both pinned, left arc */
            { rid: 2, label:'R2',  x1:0.45, y1:0.64, x2:0.45, y2:0.30, offset:8, labelSide:1 },
            /* R3   ext(left) → I1=(0.45,0.30)  →  endpoint pinned */
            { rid: 3, label:'R3',  x1:0.08, y1:0.30, x2:0.45, y2:0.30, offset:-6, labelSide:-1 },
            /* R4   ext(upper-right) → I1=(0.45,0.30)  ↙  endpoint pinned */
            { rid: 4, label:'R4',  x1:0.88, y1:0.10, x2:0.45, y2:0.30, offset:-5, labelSide:-1 },
            /* R5   I1=(0.45,0.30) → I2=(0.45,0.64)  ↓  both pinned, right arc */
            { rid: 5, label:'R5',  x1:0.45, y1:0.30, x2:0.45, y2:0.64, offset: 8, labelSide: 1 },
            /* R6   ext(left) → I2=(0.45,0.64)  →  endpoint pinned */
            { rid: 6, label:'R6',  x1:0.08, y1:0.64, x2:0.45, y2:0.64, offset: 6, labelSide: 1 },
            /* R7   I2=(0.45,0.64) → I3=(0.78,0.64)  →  both pinned */
            { rid: 7, label:'R7',  x1:0.45, y1:0.64, x2:0.78, y2:0.64, offset:-6, labelSide:-1 },
            /* R8   ext(lower-right) → I3=(0.78,0.64)  ↖  endpoint pinned */
            { rid: 8, label:'R8',  x1:0.84, y1:0.90, x2:0.78, y2:0.64, offset: 5, labelSide: 1 },
            /* R9   ext(bottom-centre) → I3=(0.78,0.64)  ↗  endpoint pinned */
            { rid: 9, label:'R9',  x1:0.62, y1:0.90, x2:0.78, y2:0.64, offset:-5, labelSide:-1 },
            /* R10  I2=(0.45,0.64) → ext(bottom)  ↓  start pinned */
            { rid:10, label:'R10', x1:0.45, y1:0.64, x2:0.45, y2:0.92, offset: 7, labelSide: 1 },
        ],
    },
};
/* ================================================================
   HELPER — congestion colour (matches road-bars thresholds)
================================================================ */
function congRoadColor(pct) {
    if (pct >= 75) return '#e74c3c';
    if (pct >= 50) return '#e67e22';
    if (pct >= 25) return '#f1c40f';
    return '#2ecc71';
}

/* ================================================================
   HELPER — apply a perpendicular pixel offset to a line segment.
   Returns { ax1, ay1, ax2, ay2 } — the shifted endpoints.

   The offset is applied in *screen* pixels after coordinate scaling,
   perpendicular to the road direction.  Positive offset = left side
   of the direction vector (x1,y1 → x2,y2).
================================================================ */
function applyOffset(sx1, sy1, sx2, sy2, offsetPx) {
    /*
     * FIX: endpoints stay pinned to their node positions.
     * Only the midpoint control is shifted perpendicularly.
     * Roads are drawn as quadratic Bézier curves so parallel
     * roads separate visually while still meeting at the same node.
     */
    const dx  = sx2 - sx1;
    const dy  = sy2 - sy1;
    const len = Math.hypot(dx, dy) || 1;
    const px  = (-dy / len) * offsetPx;
    const py  = ( dx / len) * offsetPx;
    /* Control point = midpoint + perpendicular shift */
    const cx  = (sx1 + sx2) / 2 + px;
    const cy  = (sy1 + sy2) / 2 + py;
    return { ax1: sx1, ay1: sy1, ax2: sx2, ay2: sy2, cx, cy };
}

/* ================================================================
   HELPER — draw a chevron arrow along a line at parameter t (0-1).
   The chevron is two lines forming a ">" shape aligned with the
   road direction.  Drawn in the road's colour with full opacity.
================================================================ */
function drawChevron(ctx, ax1, ay1, ax2, ay2, t, color, size, bcx, bcy) {
    /* Point on Bézier (or straight line) at parameter t */
    let cx, cy, dx, dy;
    if (bcx !== undefined) {
        /* Quadratic Bézier point: B(t) = (1-t)²P0 + 2t(1-t)Pc + t²P1 */
        const mt = 1 - t;
        cx = mt*mt*ax1 + 2*mt*t*bcx + t*t*ax2;
        cy = mt*mt*ay1 + 2*mt*t*bcy + t*t*ay2;
        /* Tangent: B'(t) = 2(1-t)(Pc-P0) + 2t(P1-Pc) */
        dx = 2*(1-t)*(bcx-ax1) + 2*t*(ax2-bcx);
        dy = 2*(1-t)*(bcy-ay1) + 2*t*(ay2-bcy);
    } else {
        cx = ax1 + (ax2 - ax1) * t;
        cy = ay1 + (ay2 - ay1) * t;
        dx = ax2 - ax1;
        dy = ay2 - ay1;
    }
    const len   = Math.hypot(dx, dy) || 1;  /* tangent length */
    const ux    = dx / len;   /* unit vector along road */
    const uy    = dy / len;
    /* Perpendicular unit vector */
    const nx    = -uy;
    const ny    =  ux;

    const half  = size * 0.55;   /* half-width of the chevron wings */
    const depth = size * 0.75;   /* how far back the wings extend */

    /* Chevron tip is slightly ahead of cx,cy; wings are behind */
    const tipX  = cx + ux * size * 0.4;
    const tipY  = cy + uy * size * 0.4;
    const lX    = cx - ux * depth + nx * half;
    const lY    = cy - uy * depth + ny * half;
    const rX    = cx - ux * depth - nx * half;
    const rY    = cy - uy * depth - ny * half;

    ctx.save();
    ctx.strokeStyle = color;
    ctx.lineWidth   = 2.2;
    ctx.lineCap     = 'round';
    ctx.lineJoin    = 'round';
    ctx.globalAlpha = 0.95;
    ctx.beginPath();
    ctx.moveTo(lX, lY);
    ctx.lineTo(tipX, tipY);
    ctx.lineTo(rX, rY);
    ctx.stroke();
    ctx.restore();
}

/* ================================================================
   HELPER — draw road label beside the road (not on top of it).

   Label is placed at parameter t along the (offset) road, then
   shifted a fixed distance perpendicular to the road on the
   requested side.  This guarantees the pill never sits on the line.
================================================================ */
function drawRoadLabel(ctx, ax1, ay1, ax2, ay2, label, pct, color, labelSide, hasData, bcx, bcy) {
    /* Midpoint on Bézier (or straight line) at t=0.5 */
    let mx, my, dx, dy;
    if (bcx !== undefined) {
        mx = 0.25*ax1 + 0.5*bcx + 0.25*ax2;
        my = 0.25*ay1 + 0.5*bcy + 0.25*ay2;
        dx = ax2 - ax1; dy = ay2 - ay1;   /* approx tangent at midpoint */
    } else {
        mx = (ax1 + ax2) / 2;
        my = (ay1 + ay2) / 2;
        dx = ax2 - ax1;
        dy = ay2 - ay1;
    }
    const len = Math.hypot(dx, dy) || 1;
    /* Perpendicular direction — pick side */
    const side = labelSide === 0 ? 1 : labelSide;
    const nx = (-dy / len) * side;
    const ny = ( dx / len) * side;

    /* Push label 14 px away from the road line */
    const PUSH = 14;
    const lx = mx + nx * PUSH;
    const ly = my + ny * PUSH;

    /* Measure text for pill sizing */
    ctx.font = 'bold 10px "Segoe UI", system-ui, sans-serif';
    const textW = ctx.measureText(label).width;
    const pillW = textW + 12;
    const pillH = 15;

    ctx.save();
    ctx.globalAlpha = 0.95;

    /* Pill background */
    ctx.fillStyle = 'rgba(10,13,22,0.88)';
    ctx.beginPath();
    ctx.roundRect(lx - pillW / 2, ly - pillH / 2, pillW, pillH, 4);
    ctx.fill();

    /* Thin border in road colour */
    ctx.strokeStyle = color + '55';   /* 33% opacity */
    ctx.lineWidth   = 0.8;
    ctx.stroke();

    /* Road ID text */
    ctx.fillStyle    = hasData ? color : '#6b7194';
    ctx.textAlign    = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillText(label, lx, ly);

    /* Congestion % on the line below — only when data available */
    if (hasData && pct !== undefined) {
        ctx.font      = '9px "Segoe UI", system-ui, sans-serif';
        ctx.fillStyle = 'rgba(190,200,220,0.75)';
        ctx.fillText(pct + '%', lx, ly + 12);
    }

    ctx.restore();
}

/* ================================================================
   MAIN DRAW FUNCTION
   Called every step change and on resize.
================================================================ */
function drawNetworkMap(stepIdx) {
    const canvas  = document.getElementById('networkCanvas');
    if (!canvas) return;
    const wrap    = document.getElementById('networkMapWrap');
    const badge   = document.getElementById('mapStepBadge');
    const pholder = document.getElementById('mapPlaceholder');

    /* ---- Size canvas to physical pixels (HiDPI aware) ---- */
    const dpr = window.devicePixelRatio || 1;
    const W   = wrap.clientWidth  || 400;
    const H   = wrap.clientHeight || 290;
    canvas.width  = W * dpr;
    canvas.height = H * dpr;
    const ctx = canvas.getContext('2d');
    ctx.scale(dpr, dpr);

    /* ---- Pick layout ---- */
    const layout = NETWORK_LAYOUTS[selectedLoc];
    if (!layout) return;

    /* ---- Congestion data for this step ---- */
    let roadCongest = {};
    let hasData = false;
    if (simData && simData.steps && stepIdx < simData.steps.length) {
        const step = simData.steps[stepIdx];
        step.roads.forEach((r, i) => { roadCongest[i + 1] = r.cong; });
        hasData = true;
        if (pholder) pholder.style.display = 'none';
        if (badge) {
            const timeMin = stepIdx * 2;
            const hh = 8 + Math.floor(timeMin / 60);
            const mm = String(timeMin % 60).padStart(2, '0');
            badge.textContent = `Step ${stepIdx}  ·  ${hh}:${mm} AM`;
        }
    } else {
        if (pholder) pholder.style.display = 'flex';
        if (badge)   badge.textContent = '—';
    }

    /* ---- Coordinate helpers ---- */
    const sx = x => x * W;   /* normalised → screen X */
    const sy = y => y * H;   /* normalised → screen Y */

    /* ================================================================
       BACKGROUND + GRID
    ================================================================ */
    ctx.fillStyle = '#090c12';
    ctx.fillRect(0, 0, W, H);

    ctx.save();
    ctx.strokeStyle = 'rgba(46,52,80,0.20)';
    ctx.lineWidth   = 1;
    const GRID = 50;
    for (let gx = 0; gx < W; gx += GRID) { ctx.beginPath(); ctx.moveTo(gx, 0); ctx.lineTo(gx, H); ctx.stroke(); }
    for (let gy = 0; gy < H; gy += GRID) { ctx.beginPath(); ctx.moveTo(0, gy); ctx.lineTo(W, gy); ctx.stroke(); }
    ctx.restore();

    /* ================================================================
       PASS 1 — GLOW HALOS (drawn first, behind everything)
       Only for roads with congestion ≥ 50 %.
    ================================================================ */
    layout.edges.forEach(e => {
        const pct = roadCongest[e.rid] ?? 0;
        if (!hasData || pct < 50) return;
        const color = congRoadColor(pct);
        const r = applyOffset(sx(e.x1), sy(e.y1), sx(e.x2), sy(e.y2), e.offset ?? 0);
        const { ax1, ay1, ax2, ay2 } = r;

        ctx.save();
        ctx.shadowColor  = color;
        ctx.shadowBlur   = pct >= 75 ? 18 : 9;
        ctx.strokeStyle  = color;
        ctx.globalAlpha  = 0.20;
        ctx.lineWidth    = 12;
        ctx.lineCap      = 'round';
        ctx.beginPath();
        ctx.moveTo(ax1, ay1);
        if (r.cx !== undefined) ctx.quadraticCurveTo(r.cx, r.cy, ax2, ay2);
        else ctx.lineTo(ax2, ay2);
        ctx.stroke();
        ctx.restore();
    });

    /* ================================================================
       PASS 2 — ROAD LINES + TERMINUS DOTS
       Each road drawn as its own offset segment.
    ================================================================ */
    layout.edges.forEach(e => {
        const pct   = roadCongest[e.rid] ?? 0;
        const color = hasData ? congRoadColor(pct) : '#2c3255';

        const r = applyOffset(sx(e.x1), sy(e.y1), sx(e.x2), sy(e.y2), e.offset ?? 0);
        const { ax1, ay1, ax2, ay2 } = r;

        /* -- Road line (Bézier keeps endpoints pinned to nodes) -- */
        ctx.save();
        ctx.strokeStyle = color;
        ctx.lineWidth   = hasData ? 3.5 : 2.5;
        ctx.lineCap     = 'round';
        ctx.globalAlpha = hasData ? 1 : 0.45;
        ctx.beginPath();
        ctx.moveTo(ax1, ay1);
        if (r.cx !== undefined) ctx.quadraticCurveTo(r.cx, r.cy, ax2, ay2);
        else ctx.lineTo(ax2, ay2);
        ctx.stroke();
        ctx.restore();

        /* -- Small dot at start endpoint (source of traffic) -- */
        ctx.save();
        ctx.fillStyle   = color;
        ctx.globalAlpha = hasData ? 0.7 : 0.3;
        ctx.beginPath();
        ctx.arc(ax1, ay1, 3, 0, Math.PI * 2);
        ctx.fill();
        ctx.restore();
    });

    /* ================================================================
       PASS 3 — DIRECTION CHEVRONS
       Two chevrons per road at 33 % and 66 % along the offset segment.
       Drawn after lines so they're always visible.
    ================================================================ */
    layout.edges.forEach(e => {
        const pct   = roadCongest[e.rid] ?? 0;
        const color = hasData ? congRoadColor(pct) : '#3a4070';

        const r = applyOffset(sx(e.x1), sy(e.y1), sx(e.x2), sy(e.y2), e.offset ?? 0);
        const { ax1, ay1, ax2, ay2 } = r;

        /* Use a slightly larger chevron on longer roads */
        const roadLen = Math.hypot(ax2 - ax1, ay2 - ay1);
        const chevSz  = Math.min(9, Math.max(6, roadLen * 0.07));

        /* Two chevrons spaced along the road (Bézier-aware) */
        drawChevron(ctx, ax1, ay1, ax2, ay2, 0.33, color, chevSz, r.cx, r.cy);
        drawChevron(ctx, ax1, ay1, ax2, ay2, 0.67, color, chevSz, r.cx, r.cy);
    });

    /* ================================================================
       PASS 4 — ROAD LABELS (beside road, not on top)
       Drawn after chevrons so labels are never obscured.
    ================================================================ */
    layout.edges.forEach(e => {
        const pct   = roadCongest[e.rid] ?? 0;
        const color = hasData ? congRoadColor(pct) : '#4a5080';

        const r = applyOffset(sx(e.x1), sy(e.y1), sx(e.x2), sy(e.y2), e.offset ?? 0);
        const { ax1, ay1, ax2, ay2 } = r;

        drawRoadLabel(ctx, ax1, ay1, ax2, ay2,
                      e.label, pct, color,
                      e.labelSide ?? 1, hasData, r.cx, r.cy);
    });

    /* ================================================================
       PASS 5 — INTERSECTION NODES
       Drawn last so they always sit on top of roads.
    ================================================================ */
    const NODE_R = Math.min(22, Math.max(16, W * 0.038));

    layout.nodes.forEach(n => {
        const cx = sx(n.x);
        const cy = sy(n.y);

        /* Ambient glow ring */
        ctx.save();
        const grad = ctx.createRadialGradient(cx, cy, NODE_R, cx, cy, NODE_R + 10);
        grad.addColorStop(0, 'rgba(79,142,247,0.22)');
        grad.addColorStop(1, 'rgba(79,142,247,0)');
        ctx.fillStyle = grad;
        ctx.beginPath();
        ctx.arc(cx, cy, NODE_R + 10, 0, Math.PI * 2);
        ctx.fill();
        ctx.restore();

        /* Node body */
        ctx.save();
        ctx.beginPath();
        ctx.arc(cx, cy, NODE_R, 0, Math.PI * 2);
        ctx.fillStyle   = '#12162a';
        ctx.strokeStyle = '#4f8ef7';
        ctx.lineWidth   = 2;
        ctx.fill();
        ctx.stroke();
        ctx.restore();

        /* Node label — two lines (ID + short name) */
        const lines = n.label.split('\n');
        ctx.save();
        ctx.textAlign    = 'center';
        ctx.textBaseline = 'middle';
        lines.forEach((line, li) => {
            const isId  = li === 0;
            ctx.font      = isId
                ? `bold ${Math.round(NODE_R * 0.55)}px "Segoe UI", system-ui, sans-serif`
                : `${Math.round(NODE_R * 0.40)}px "Segoe UI", system-ui, sans-serif`;
            ctx.fillStyle = isId ? '#7ab3ff' : '#8590b0';
            const totalH  = lines.length * (NODE_R * 0.52);
            const yOff    = (li - (lines.length - 1) / 2) * (NODE_R * 0.52);
            ctx.fillText(line, cx, cy + yOff);
        });
        ctx.restore();
    });
}

/* ---- Public: called from renderStep() — canvas is always visible now ---- */
function updateNetworkMap() {
    drawNetworkMap(currentStep);
}

/* ---- Redraw on window resize ---- */
let _resizeTimer;
window.addEventListener('resize', () => {
    clearTimeout(_resizeTimer);
    _resizeTimer = setTimeout(() => drawNetworkMap(currentStep), 120);
});

/* ---- Draw skeleton map on page load so the canvas isn't blank ---- */
window.addEventListener('DOMContentLoaded', () => {
    /* Small delay lets CSS finish painting so clientWidth/Height are correct */
    setTimeout(() => drawNetworkMap(currentStep), 80);
});