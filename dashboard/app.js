// SurgiSafe-RT dashboard.
//
// Scenario: a remote, AI-assisted ultrasound robot. The workspace is a top
// view of the patient; the pink scan site is where the probe touches the
// skin (pressure limit), and the red circle is a surgical wound the probe
// must never enter (no-go zone).
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
      $('freeze-btn').textContent = `Network lag: freeze ${freezeMs()} ms`;
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
    const why = REASON_TEXT[s.reason] ? ` ${REASON_TEXT[s.reason]}.` : '';
    setStatus('stop', 'SAFE STOP', `Reason: ${s.reason}${extra}.${why} Probe lifted; motion latched off until operator reset.`);
  } else if (s.controller === 'frozen' || s.controller === 'killed') {
    setStatus('hold', 'Controller silent', `Controller ${s.controller}; watchdog counting down…`);
  } else if (s.holding) {
    setStatus('hold', 'HOLD', 'No fresh command from the operator or AI. Holding position safely.');
  } else {
    setStatus('ok', 'MOTION OK', 'Following the operator / AI within every limit.');
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
  rtOnState(s);
}

// Plain-language meaning of each stop reason in the ultrasound scenario.
const REASON_TEXT = {
  'watchdog timeout': 'The commands went silent (lag, freeze or crash)',
  'contact force': 'The probe pressed on the skin too hard',
  'no-go zone': 'The probe was about to enter the surgical wound',
  'speed limit': 'A command asked the arm to move too fast',
  'acceleration limit': 'A command asked for a sudden, jerky change of motion',
  'out of workspace': 'A command pointed outside the patient area',
  'invalid sequence': 'Commands arrived out of order (replayed or skipped)',
};

// Freeze long enough to pass the watchdog deadline (15 ms on hardware,
// 50 ms on the QEMU profile).
function freezeMs() {
  return env ? Math.max(20, Math.ceil(env.timeout_ms * 2 / 10) * 10) : 20;
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
    ctx.fillText('scan site', c.x, c.y + 4);
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
    ctx.fillText('wound', c.x, c.y - 2);
    ctx.font = '11px -apple-system, sans-serif';
    ctx.fillText('no-scan zone', c.x, c.y + 12);
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
  mouse: 'Move the mouse over the patient. The probe follows, within the limits.',
  hand: 'Your index fingertip steers the probe, as a remote sonographer would. Hide your hand and the arm holds still.',
  auto: 'An AI auto-scan sweeps the probe in circles over a safe area on its own.',
  drift: 'The AI has a planning bug: its sweep slowly drifts toward the surgical wound.',
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
    if (f === 'freeze') send({ cmd: 'freeze', ms: freezeMs() });
    if (f === 'kill') send({ cmd: 'kill' });
    if (f === 'rogue') send({ cmd: 'rogue', on: true });
  });
});

$('reset').addEventListener('click', () => {
  send({ cmd: 'reset' });
  trail.length = 0;
  autoStart = performance.now();  // Restart the autopilot's plan.
});

// ------------------------------------------------------ real-time proof

// Per-cycle timing from the server, reduced to the worst value per 100 ms
// bucket for the chart, plus worst-ever values without and with CPU stress.
const RT_WINDOW_S = 20;
const RT_BUCKET_MS = 100;
const rtBuckets = [];  // { t, decide, late, stress } — values in µs.
const rtCmp = {
  off: { decide: 0, late: 0, n: 0 },
  on: { decide: 0, late: 0, n: 0 },
};
let rtStress = false;

function fmtUs(us) {
  if (!(us >= 0)) return '–';
  return us >= 1000 ? `${(us / 1000).toFixed(2)} ms` : `${Math.round(us)} µs`;
}

function rtOnState(s) {
  if (s.decide_us === undefined) return;
  const now = performance.now();
  const key = Math.floor(now / RT_BUCKET_MS) * RT_BUCKET_MS;
  let b = rtBuckets[rtBuckets.length - 1];
  if (!b || b.t !== key) {
    b = { t: key, decide: 0, late: 0, stress: false };
    rtBuckets.push(b);
    while (rtBuckets.length && rtBuckets[0].t < now - RT_WINDOW_S * 1000) rtBuckets.shift();
  }
  b.decide = Math.max(b.decide, s.decide_us);
  b.late = Math.max(b.late, s.late_us);
  b.stress = b.stress || s.stress;

  const c = s.stress ? rtCmp.on : rtCmp.off;
  c.decide = Math.max(c.decide, s.decide_us);
  c.late = Math.max(c.late, s.late_us);
  c.n++;

  rtStress = s.stress;
  const btn = $('stress');
  btn.classList.toggle('on', s.stress);
  btn.textContent = s.stress
    ? `Stop the stress (${Math.ceil(s.stress_left)} s left)`
    : 'Stress the CPU (60 s)';
  $('rt-load').textContent = s.stress
    ? `${s.stress_threads} threads${s.stress_low_prio ? ' · prio 5' : ''}` : 'off';
  $('rt-load').title = s.stress
    ? `${s.stress_mops.toFixed(0)} million loop iterations per second` : '';
}

// Updated a few times per second, not per message.
setInterval(() => {
  const recent = rtBuckets.filter((b) => b.t >= performance.now() - 1000);
  const worst = (k) => recent.reduce((m, b) => Math.max(m, b[k]), 0);
  if (recent.length) {
    $('rt-decide').textContent = fmtUs(worst('decide'));
    $('rt-late').textContent = fmtUs(worst('late'));
  }
  for (const k of ['off', 'on']) {
    const c = rtCmp[k];
    $(`cmp-decide-${k}`).textContent = c.n ? fmtUs(c.decide) : '–';
    $(`cmp-late-${k}`).textContent = c.n ? fmtUs(c.late) : '–';
    $(`cmp-n-${k}`).textContent = c.n.toLocaleString();
  }
  drawRt();
}, 200);

const rtCanvas = $('rt-canvas');
const rctx = rtCanvas.getContext('2d');
let rtHoverX = null;

function niceCeil(v) {
  const steps = [0.5, 1, 2, 5, 10, 20, 50, 100, 200];
  return steps.find((s) => s >= v) || Math.ceil(v / 100) * 100;
}

function drawRt() {
  const W = rtCanvas.width, H = rtCanvas.height;
  const L = 84, R = 24, T = 12, B = 30;
  const pw = W - L - R, ph = H - T - B;
  const now = performance.now();
  const t0 = now - RT_WINDOW_S * 1000;
  const css = getComputedStyle(rtCanvas.closest('.rt'));
  const cDecide = css.getPropertyValue('--decide').trim();
  const cLate = css.getPropertyValue('--late').trim();
  rctx.clearRect(0, 0, W, H);

  // Y axis in ms, shared by both series (same unit).
  const maxMs = rtBuckets.reduce((m, b) => Math.max(m, b.decide, b.late), 0) / 1000;
  const top = niceCeil(Math.max(1, maxMs * 1.1));
  const x = (t) => L + ((t - t0) / (RT_WINDOW_S * 1000)) * pw;
  const y = (us) => T + ph - Math.min(1, us / 1000 / top) * ph;

  // Stress bands.
  rctx.fillStyle = 'rgba(139, 154, 165, 0.16)';
  for (const b of rtBuckets) {
    if (b.stress) rctx.fillRect(x(b.t), T, Math.max(1, x(b.t + RT_BUCKET_MS) - x(b.t)), ph);
  }

  // Grid + labels (recessive).
  rctx.font = '20px -apple-system, sans-serif';
  rctx.fillStyle = '#8b9aa5';
  rctx.strokeStyle = '#24303a';
  rctx.lineWidth = 1;
  rctx.textAlign = 'right';
  for (let i = 0; i <= 4; i++) {
    const v = (top * i) / 4;
    const yy = T + ph - (ph * i) / 4;
    rctx.beginPath(); rctx.moveTo(L, yy); rctx.lineTo(W - R, yy); rctx.stroke();
    rctx.fillText(`${+v.toFixed(2)} ms`, L - 6, yy + 6);
  }
  rctx.textAlign = 'center';
  for (let sAgo = RT_WINDOW_S; sAgo >= 0; sAgo -= 5) {
    rctx.textAlign = sAgo === 0 ? 'right' : 'center';
    rctx.fillText(sAgo ? `−${sAgo} s` : 'now', sAgo === 0 ? W - R : x(now - sAgo * 1000), H - 6);
  }

  // Series: 2px lines.
  for (const [key, color] of [['late', cLate], ['decide', cDecide]]) {
    rctx.beginPath();
    rtBuckets.forEach((b, i) => {
      const px = x(b.t + RT_BUCKET_MS / 2), py = y(b[key]);
      i ? rctx.lineTo(px, py) : rctx.moveTo(px, py);
    });
    rctx.strokeStyle = color;
    rctx.lineWidth = 2.5;
    rctx.lineJoin = 'round';
    rctx.stroke();
  }

  // Hover crosshair + tooltip.
  const tip = $('rt-tip');
  if (rtHoverX === null || !rtBuckets.length) { tip.hidden = true; return; }
  const th = t0 + ((rtHoverX - L) / pw) * RT_WINDOW_S * 1000;
  let best = null;
  for (const b of rtBuckets) {
    if (!best || Math.abs(b.t - th) < Math.abs(best.t - th)) best = b;
  }
  if (!best) { tip.hidden = true; return; }
  const hx = x(best.t + RT_BUCKET_MS / 2);
  rctx.strokeStyle = '#8b9aa5';
  rctx.lineWidth = 1;
  rctx.beginPath(); rctx.moveTo(hx, T); rctx.lineTo(hx, T + ph); rctx.stroke();
  for (const [key, color] of [['late', cLate], ['decide', cDecide]]) {
    rctx.beginPath();
    rctx.arc(hx, y(best[key]), 5, 0, Math.PI * 2);
    rctx.fillStyle = color; rctx.fill();
    rctx.strokeStyle = '#171e24'; rctx.lineWidth = 2; rctx.stroke();
  }
  const ago = ((now - best.t) / 1000).toFixed(1);
  tip.textContent = `${ago} s ago${best.stress ? ' · stress on' : ''} — decision ${fmtUs(best.decide)}, start delay ${fmtUs(best.late)}`;
  tip.hidden = false;
  const scale = rtCanvas.getBoundingClientRect().width / W;
  const left = hx * scale;
  tip.style.left = `${Math.max(0, Math.min(left - tip.offsetWidth / 2, rtCanvas.getBoundingClientRect().width - tip.offsetWidth))}px`;
}

rtCanvas.addEventListener('mousemove', (e) => {
  const r = rtCanvas.getBoundingClientRect();
  rtHoverX = (e.clientX - r.left) * (rtCanvas.width / r.width);
  drawRt();
});
rtCanvas.addEventListener('mouseleave', () => { rtHoverX = null; drawRt(); });

$('stress').addEventListener('click', () => send({ cmd: 'stress', on: !rtStress }));

$('story-close').addEventListener('click', () => { $('story').hidden = true; });

connect();
draw();
