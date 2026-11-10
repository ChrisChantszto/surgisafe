// SurgiSafe-RT dashboard.
//
// This page is the *untrusted planner* side: it picks targets (mouse, hand
// tracking, or an autopilot) and sends them to the simulator / QNX gateway.
// It never decides whether a move is safe; it only displays what the
// safety monitor decided.

const canvas = document.getElementById('workspace');
const ctx = canvas.getContext('2d');
const $ = (id) => document.getElementById(id);

let env = null;          // Envelope from the server.
let state = null;        // Latest telemetry.
let ws = null;
let mode = 'mouse';
const trail = [];
const TRAIL_MAX = 600;

// ---------------------------------------------------------------- network

function connect() {
  const url = `ws://${location.host || 'localhost:8080'}/ws`;
  ws = new WebSocket(url);
  ws.onopen = () => setConn(true);
  ws.onclose = () => {
    setConn(false);
    setStatus('off', 'Simulator not connected', 'Start it with make demo; retrying…');
    setTimeout(connect, 1000);
  };
  ws.onmessage = (ev) => {
    let msg;
    try { msg = JSON.parse(ev.data); } catch { return; }
    if (msg.type === 'envelope') {
      env = msg;
      const el = $('conn');
      el.textContent = msg.source === 'qnx'
        ? 'Connected · decisions by QNX 8.0 safety monitor'
        : 'Connected · Mac simulator';
    }
    else if (msg.type === 'state') onState(msg);
    else if (msg.type === 'event') log(msg.level, msg.text);
  };
}

function send(obj) {
  if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
}

function sendTarget(p) {
  if (p) send({ cmd: 'target', x: +p.x.toFixed(4), y: +p.y.toFixed(4) });
}

function setConn(on) {
  const el = $('conn');
  el.textContent = on ? 'Connected' : 'Disconnected';
  el.className = 'pill ' + (on ? 'pill-on' : 'pill-off');
}

// --------------------------------------------------------------- telemetry

function onState(s) {
  const wasStopped = state && state.stopped;
  state = s;
  if (!wasStopped || trail.length === 0) {
    const last = trail[trail.length - 1];
    if (!last || Math.hypot(last.x - s.x, last.y - s.y) > 1e-4) {
      trail.push({ x: s.x, y: s.y });
      if (trail.length > TRAIL_MAX) trail.shift();
    }
  }

  if (s.stopped) {
    const extra = s.reason === 'watchdog timeout' && s.silence_ms
      ? ` (${s.silence_ms.toFixed(1)} ms of silence)` : '';
    setStatus('stop', 'SAFE STOP', `Reason: ${s.reason}${extra}. Motion latched off until operator reset.`);
  } else if (s.controller === 'frozen' || s.controller === 'killed') {
    setStatus('hold', 'Controller silent', `Controller ${s.controller}; watchdog counting down…`);
  } else if (s.holding) {
    setStatus('hold', 'HOLD', 'No fresh target from the planner. Holding position safely.');
  } else {
    setStatus('ok', 'MOTION OK', 'Following the planner target within every limit.');
  }
  $('reset').disabled = !s.stopped;
  // Faults only make sense while motion is enabled.
  document.querySelectorAll('[data-fault]').forEach((b) => {
    b.disabled = s.stopped || s.controller !== 'running';
  });

  $('s-cycle').textContent = s.cycle;
  $('s-accepted').textContent = s.accepted.toLocaleString();
  const speed = env && !s.stopped ? Math.hypot(s.vx, s.vy) / env.max_step : 0;
  $('s-speed').textContent = `${Math.round(speed * 100)}%`;
  $('s-controller').textContent = s.controller;
  updateForce(s);
}

let forceSeen = false;
function updateForce(s) {
  if (!env || !env.max_force) return;
  const f = s.force || 0;
  if (f > 0) forceSeen = true;
  const span = env.max_force * 2;  // Bar shows 0 to twice the limit.
  $('force-val').textContent = `${f.toFixed(1)} N / limit ${env.max_force.toFixed(1)} N`;
  $('force-fill').style.width = `${Math.min(100, (f / span) * 100)}%`;
  $('force-fill').style.background = f > env.max_force ? '#e5534b' : f > 0.6 * env.max_force ? '#f0a33a' : '#2fb38a';
  $('force-limit').style.left = '50%';
  if (forceSeen) $('force-note').textContent = 'Measured at the instrument tip by the MuJoCo physics simulation';
}

function setStatus(kind, label, detail) {
  $('status').className = `card status status-${kind}`;
  $('status-label').textContent = label;
  $('status-detail').textContent = detail;
}

function log(level, text) {
  const li = document.createElement('li');
  li.className = level;
  const t = document.createElement('time');
  t.textContent = new Date().toLocaleTimeString();
  li.append(t, text);
  const list = $('log');
  list.prepend(li);
  while (list.children.length > 100) list.lastChild.remove();
}

// ----------------------------------------------------------------- drawing

const PAD = 32;
function toPx(p) {
  const w = canvas.width - 2 * PAD, h = canvas.height - 2 * PAD;
  return {
    x: PAD + (p.x - env.min_x) / (env.max_x - env.min_x) * w,
    y: PAD + (1 - (p.y - env.min_y) / (env.max_y - env.min_y)) * h,
  };
}
function toWorld(px, py) {
  const w = canvas.width - 2 * PAD, h = canvas.height - 2 * PAD;
  return {
    x: env.min_x + (px - PAD) / w * (env.max_x - env.min_x),
    y: env.min_y + (1 - (py - PAD) / h) * (env.max_y - env.min_y),
  };
}
function scale() { return (canvas.width - 2 * PAD) / (env.max_x - env.min_x); }

function draw() {
  requestAnimationFrame(draw);
  ctx.clearRect(0, 0, canvas.width, canvas.height);
  if (!env) return;

  // Grid and bounds.
  ctx.strokeStyle = '#24303a';
  ctx.lineWidth = 1;
  for (let i = Math.ceil(env.min_x); i <= env.max_x; i++) {
    const a = toPx({ x: i, y: env.min_y }), b = toPx({ x: i, y: env.max_y });
    ctx.beginPath(); ctx.moveTo(a.x, a.y); ctx.lineTo(b.x, b.y); ctx.stroke();
  }
  for (let j = Math.ceil(env.min_y); j <= env.max_y; j++) {
    const a = toPx({ x: env.min_x, y: j }), b = toPx({ x: env.max_x, y: j });
    ctx.beginPath(); ctx.moveTo(a.x, a.y); ctx.lineTo(b.x, b.y); ctx.stroke();
  }
  const tl = toPx({ x: env.min_x, y: env.max_y });
  ctx.strokeStyle = '#4b5a65';
  ctx.lineWidth = 2;
  ctx.strokeRect(tl.x, tl.y, canvas.width - 2 * PAD, canvas.height - 2 * PAD);

  // Tissue mound in the MuJoCo scene (sim/mujoco/scene.py TISSUE). It is
  // not part of the envelope: touching it is allowed, pushing too hard
  // is not (contact-force limit).
  {
    const c = toPx({ x: 2.5, y: 5.5 });
    ctx.beginPath();
    ctx.arc(c.x, c.y, 0.9 * scale(), 0, Math.PI * 2);
    ctx.fillStyle = 'rgba(232, 140, 153, 0.22)';
    ctx.fill();
    ctx.strokeStyle = '#e88c99';
    ctx.lineWidth = 1.5;
    ctx.stroke();
    ctx.fillStyle = '#e88c99';
    ctx.font = '12px -apple-system, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('tissue', c.x, c.y + 4);
  }

  // No-go zones.
  for (const z of env.zones) {
    const c = toPx(z);
    ctx.beginPath();
    ctx.arc(c.x, c.y, z.r * scale(), 0, Math.PI * 2);
    ctx.fillStyle = 'rgba(229, 83, 75, 0.18)';
    ctx.fill();
    ctx.setLineDash([6, 5]);
    ctx.strokeStyle = '#e5534b';
    ctx.lineWidth = 1.5;
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = '#e5534b';
    ctx.font = '12px -apple-system, sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('no-go zone', c.x, c.y + 4);
  }

  if (!state) return;

  // Trail.
  if (trail.length > 1) {
    ctx.beginPath();
    trail.forEach((p, i) => {
      const q = toPx(p);
      i ? ctx.lineTo(q.x, q.y) : ctx.moveTo(q.x, q.y);
    });
    ctx.strokeStyle = 'rgba(47, 179, 138, 0.45)';
    ctx.lineWidth = 2;
    ctx.stroke();
  }

  // Target.
  if (state.has_target) {
    const t = toPx({ x: state.tx, y: state.ty });
    ctx.strokeStyle = '#5aa9e6';
    ctx.lineWidth = 2;
    ctx.beginPath();
    ctx.moveTo(t.x - 9, t.y); ctx.lineTo(t.x + 9, t.y);
    ctx.moveTo(t.x, t.y - 9); ctx.lineTo(t.x, t.y + 9);
    ctx.stroke();
  }

  // Tip.
  const p = toPx(state);
  const color = state.stopped ? '#e5534b' : state.holding ? '#f0a33a' : '#2fb38a';
  if (state.stopped) {
    ctx.beginPath();
    ctx.arc(p.x, p.y, 18, 0, Math.PI * 2);
    ctx.strokeStyle = 'rgba(229, 83, 75, 0.6)';
    ctx.lineWidth = 3;
    ctx.stroke();
  }
  ctx.beginPath();
  ctx.arc(p.x, p.y, 8, 0, Math.PI * 2);
  ctx.fillStyle = color;
  ctx.fill();

  // Velocity indicator.
  const v = { x: state.x + state.vx * 8, y: state.y + state.vy * 8 };
  const vp = toPx(v);
  ctx.beginPath(); ctx.moveTo(p.x, p.y); ctx.lineTo(vp.x, vp.y);
  ctx.strokeStyle = color; ctx.lineWidth = 2; ctx.stroke();
}

// --------------------------------------------------------- planner: mouse

let mouseTarget = null;
canvas.addEventListener('mousemove', (e) => {
  if (!env || mode !== 'mouse') return;
  const r = canvas.getBoundingClientRect();
  const px = (e.clientX - r.left) * (canvas.width / r.width);
  const py = (e.clientY - r.top) * (canvas.height / r.height);
  mouseTarget = toWorld(px, py);
});
canvas.addEventListener('mouseleave', () => { mouseTarget = null; });

// ------------------------------------------------------ planner: autopilot

// A simple "AI" planner that orbits a safe region. The "with error" variant
// lets its orbit drift toward the no-go zone, as a planning bug would.
let autoStart = 0;
function autopilotTarget(drift) {
  const t = (performance.now() - autoStart) / 1000;
  let cx = 2.8, cy = 2.8;
  if (drift) {
    const k = Math.min(1, t / 20);  // Drifts over about 20 s.
    cx += k * 2.2; cy += k * 2.2;
  }
  const r = 1.8, w = (2 * Math.PI) / 8;
  return { x: cx + r * Math.cos(w * t), y: cy + r * Math.sin(w * t) };
}

// ------------------------------------------------- planner: hand tracking

// Google MediaPipe hand landmarker, running locally in the browser. Video
// frames never leave this machine; the model files are fetched once.
const MP_VERSION = '0.10.14';
const MP_BASE = `https://cdn.jsdelivr.net/npm/@mediapipe/tasks-vision@${MP_VERSION}`;
const MP_MODEL = 'https://storage.googleapis.com/mediapipe-models/hand_landmarker/hand_landmarker/float16/1/hand_landmarker.task';

let landmarker = null;
let stream = null;
let handTarget = null;
let lastVideoTime = -1;

async function startHand() {
  const msg = $('camera-msg');
  $('camera').hidden = false;
  try {
    msg.textContent = 'Loading hand-tracking model…';
    if (!landmarker) {
      const vision = await import(`${MP_BASE}/vision_bundle.mjs`);
      const files = await vision.FilesetResolver.forVisionTasks(`${MP_BASE}/wasm`);
      landmarker = await vision.HandLandmarker.createFromOptions(files, {
        baseOptions: { modelAssetPath: MP_MODEL, delegate: 'GPU' },
        runningMode: 'VIDEO',
        numHands: 1,
      });
    }
    msg.textContent = 'Allow camera access…';
    stream = await navigator.mediaDevices.getUserMedia({ video: { width: 640, height: 480 } });
    const video = $('video');
    video.srcObject = stream;
    await video.play();
    msg.textContent = 'Show one hand; your index fingertip is the target';
    trackHand();
  } catch (err) {
    msg.textContent = `Hand tracking unavailable: ${err.message || err}. Try Mouse mode.`;
  }
}

function stopHand() {
  if (stream) stream.getTracks().forEach((t) => t.stop());
  stream = null;
  handTarget = null;
  $('camera').hidden = true;
}

function trackHand() {
  if (mode !== 'hand' || !stream) return;
  const video = $('video');
  const overlay = $('overlay');
  const octx = overlay.getContext('2d');
  if (video.currentTime !== lastVideoTime && video.readyState >= 2) {
    lastVideoTime = video.currentTime;
    const res = landmarker.detectForVideo(video, performance.now());
    octx.clearRect(0, 0, overlay.width, overlay.height);
    if (res.landmarks && res.landmarks.length) {
      const lm = res.landmarks[0];
      for (const p of lm) {
        octx.fillStyle = 'rgba(90, 169, 230, 0.8)';
        octx.fillRect(p.x * overlay.width - 2, p.y * overlay.height - 2, 4, 4);
      }
      const tip = lm[8];  // Index fingertip.
      octx.beginPath();
      octx.arc(tip.x * overlay.width, tip.y * overlay.height, 7, 0, Math.PI * 2);
      octx.strokeStyle = '#2fb38a';
      octx.lineWidth = 3;
      octx.stroke();
      // Mirror horizontally, and use the central 80% of the frame so the
      // whole workspace is reachable without leaving the camera's view.
      const u = Math.min(1, Math.max(0, ((1 - tip.x) - 0.1) / 0.8));
      const v = Math.min(1, Math.max(0, ((1 - tip.y) - 0.1) / 0.8));
      handTarget = {
        x: env.min_x + u * (env.max_x - env.min_x),
        y: env.min_y + v * (env.max_y - env.min_y),
      };
      $('camera-msg').textContent = '';
    } else {
      handTarget = null;  // No hand: stop sending, controller will hold.
      $('camera-msg').textContent = 'No hand detected: holding';
    }
  }
  requestAnimationFrame(trackHand);
}

// --------------------------------------------------- planner send loop

// Targets are sent at about 30 Hz. If the planner has nothing to say (mouse
// outside the canvas, no hand in view) it sends nothing, and the controller
// brakes and holds after 250 ms.
setInterval(() => {
  if (!env) return;
  if (mode === 'mouse') sendTarget(mouseTarget);
  else if (mode === 'hand') sendTarget(handTarget);
  else if (mode === 'auto') sendTarget(autopilotTarget(false));
  else if (mode === 'drift') sendTarget(autopilotTarget(true));
}, 33);

// ----------------------------------------------------------------- controls

const hints = {
  mouse: 'Move the mouse over the workspace. The tip follows, within the limits.',
  hand: 'Your index fingertip steers the tip. Hide your hand and the controller holds.',
  auto: 'An AI autopilot orbits a safe region on its own.',
  drift: 'The autopilot has a planning bug: its orbit drifts toward the no-go zone.',
};

document.querySelectorAll('.segmented button').forEach((b) => {
  b.addEventListener('click', () => {
    mode = b.dataset.mode;
    document.querySelectorAll('.segmented button').forEach((o) => {
      o.classList.toggle('active', o === b);
      o.setAttribute('aria-checked', o === b ? 'true' : 'false');
    });
    $('mode-hint').textContent = hints[mode];
    autoStart = performance.now();
    if (mode === 'hand') startHand(); else stopHand();
  });
});

document.querySelectorAll('[data-fault]').forEach((b) => {
  b.addEventListener('click', () => {
    const f = b.dataset.fault;
    if (f === 'freeze') send({ cmd: 'freeze', ms: 20 });
    if (f === 'kill') send({ cmd: 'kill' });
    if (f === 'rogue') send({ cmd: 'rogue', on: true });
  });
});

$('reset').addEventListener('click', () => {
  send({ cmd: 'reset' });
  trail.length = 0;
  autoStart = performance.now();  // Restart the autopilot's plan.
});

connect();
draw();
