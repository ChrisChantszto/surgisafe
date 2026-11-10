#!/usr/bin/env python3
"""Builds a self-contained HTML report from QNX scenario results.

    make qnx-scenarios REPEATS=4      # run on the QNX target
    make qnx-report                   # fetch results + build this report
    open build/qnx-report.html

Input:  build/qnx-results/{meta.txt, summary.csv, <scenario>_<run>.csv}
        (written by qnx/vm/scenarios_on_target.sh and controller --trace)
Output: build/qnx-report.html (no network access needed to view it)

Standard library only.
"""

import csv
import html
import json
import os
import sys

# Mirrors the timing profiles and default envelope in safety_core.cpp.
PROFILES = {
    "hardware": {"period": 10, "deadline": 15, "margin": 3},
    "qemu": {"period": 10, "deadline": 50, "margin": 20},
}
ENVELOPE = {"min": 0.0, "max": 8.0, "zones": [{"x": 5.0, "y": 5.0, "r": 1.0}]}
REASONS = {0: "accepted", 1: "watchdog timeout", 2: "out of workspace",
           3: "invalid sequence", 4: "monitor fault", 5: "speed limit",
           6: "acceleration limit", 7: "no-go zone"}


def load(results_dir):
    meta = {}
    with open(os.path.join(results_dir, "meta.txt")) as f:
        for line in f:
            if "=" in line:
                k, v = line.rstrip("\n").split("=", 1)
                meta[k] = v
    with open(os.path.join(results_dir, "summary.csv")) as f:
        summary = list(csv.DictReader(f))
    traces = {}
    for row in summary:
        key = f"{row['scenario']}_{row['run']}"
        path = os.path.join(results_dir, key + ".csv")
        if not os.path.exists(path):
            continue
        cols = {"t": [], "late": [], "rtt": [], "x": [], "y": [], "ok": [],
                "reason": []}
        with open(path) as f:
            for r in csv.DictReader(f):
                cols["t"].append(round(float(r["t_ms"]), 3))
                cols["late"].append(round(float(r["late_ms"]), 3))
                cols["rtt"].append(round(float(r["rtt_ms"]), 3))
                cols["x"].append(round(float(r["x"]), 4))
                cols["y"].append(round(float(r["y"]), 4))
                cols["ok"].append(int(r["permitted"]))
                cols["reason"].append(int(r["reason"]))
        traces[key] = cols
    return meta, summary, traces


PAGE = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SurgiSafe-RT on QNX: scenario report</title>
<style>
.viz-root {
  color-scheme: light;
  --surface-0: #f4f3f0; --surface-1: #fcfcfb; --border: #e2e1dc;
  --text-primary: #0b0b0b; --text-secondary: #52514e; --text-muted: #7a7974;
  --grid: #ecebe7; --axis: #b9b8b2;
  --series-1: #2a78d6;
  --good: #0ca30c; --critical: #d03b3b; --good-text: #006300;
  --zone-fill: rgba(208, 59, 59, 0.12);
}
@media (prefers-color-scheme: dark) {
  :root:where(:not([data-theme="light"])) .viz-root {
    color-scheme: dark;
    --surface-0: #121211; --surface-1: #1a1a19; --border: #2e2e2b;
    --text-primary: #f0efec; --text-secondary: #c3c2b7; --text-muted: #8f8e86;
    --grid: #262624; --axis: #4a4a46;
    --series-1: #3987e5;
    --good-text: #4cc34c;
    --zone-fill: rgba(208, 59, 59, 0.20);
  }
}
* { box-sizing: border-box; }
body { margin: 0; }
.viz-root { background: var(--surface-0); color: var(--text-primary);
  font: 14px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  min-height: 100vh; padding: 24px; }
.wrap { max-width: 1120px; margin: 0 auto; }
h1 { font-size: 22px; font-weight: 600; margin: 0; }
h2 { font-size: 15px; font-weight: 600; margin: 0 0 2px; }
.sub { color: var(--text-secondary); margin: 4px 0 0; }
.note { color: var(--text-muted); font-size: 12px; margin: 2px 0 12px; }
.tiles { display: grid; grid-template-columns: repeat(4, 1fr); gap: 12px; margin: 20px 0; }
.tile, .card { background: var(--surface-1); border: 1px solid var(--border); border-radius: 12px; }
.tile { padding: 14px 16px; }
.tile .label { color: var(--text-secondary); font-size: 12px; }
.tile .value { font-size: 26px; font-weight: 600; font-variant-numeric: tabular-nums; }
.tile .hint { color: var(--text-muted); font-size: 12px; }
.card { padding: 16px 18px; margin-bottom: 16px; }
.grid2 { display: grid; grid-template-columns: 3fr 2fr; gap: 16px; }
.controls { display: flex; gap: 12px; align-items: center; margin-bottom: 12px; flex-wrap: wrap; }
.controls label { color: var(--text-secondary); font-size: 13px; }
select { font: inherit; color: var(--text-primary); background: var(--surface-0);
  border: 1px solid var(--border); border-radius: 8px; padding: 4px 8px; }
table { border-collapse: collapse; width: 100%; font-size: 13px; }
th { text-align: left; color: var(--text-secondary); font-weight: 500;
  border-bottom: 1px solid var(--border); padding: 6px 8px; }
td { padding: 6px 8px; border-bottom: 1px solid var(--grid); font-variant-numeric: tabular-nums; }
tr.sel td { background: var(--grid); }
tr.click { cursor: pointer; }
td.runs { white-space: nowrap; }
.cell { display: inline-flex; align-items: center; justify-content: center;
  width: 22px; height: 22px; border-radius: 6px; margin-right: 3px;
  font-size: 13px; font-weight: 700; color: #fff; }
.cell.ok { background: var(--good); }
.cell.bad { background: var(--critical); }
.status { font-weight: 600; }
.status.ok { color: var(--good-text); }
.status.bad { color: var(--critical); }
svg text { fill: var(--text-secondary); font-size: 11px; }
svg .title-ink { fill: var(--text-primary); }
.tip { position: fixed; pointer-events: none; background: var(--surface-1);
  color: var(--text-primary); border: 1px solid var(--border); border-radius: 8px;
  padding: 6px 9px; font-size: 12px; box-shadow: 0 4px 14px rgba(0,0,0,.12);
  display: none; z-index: 10; white-space: nowrap; }
.legend { display: flex; gap: 16px; flex-wrap: wrap; color: var(--text-secondary); font-size: 12px; margin-top: 6px; }
.legend i { display: inline-block; vertical-align: middle; margin-right: 6px; }
details { margin-top: 10px; }
summary { cursor: pointer; color: var(--text-secondary); font-size: 13px; }
@media (max-width: 900px) { .tiles { grid-template-columns: repeat(2, 1fr); } .grid2 { grid-template-columns: 1fr; } }
</style>
</head>
<body>
<div class="viz-root">
<div class="wrap">
  <h1>SurgiSafe-RT on QNX 8.0: fault-injection report</h1>
  <p class="sub" id="meta"></p>
  <p class="note">Each scenario runs through the real QNX processes: the controller proposes moves with
  <code>MsgSend</code>, the safety monitor decides and enforces its heartbeat deadline with a QNX timer pulse.
  Under QEMU software emulation timing is <b>not</b> real-time, so the <code>qemu</code> profile uses a wider deadline.</p>

  <div class="tiles" id="tiles"></div>

  <div class="card">
    <h2>Did every fault get caught, for the right reason, at the right time?</h2>
    <p class="note">One square per run. Click a row to inspect that scenario below.</p>
    <table id="matrix"></table>
  </div>

  <div class="card">
    <div class="controls">
      <label>Scenario <select id="sel-scenario"></select></label>
      <label>Run <select id="sel-run"></select></label>
      <span class="note" id="sel-desc" style="margin:0"></span>
    </div>
    <div class="grid2">
      <div>
        <h2>Heartbeat gaps the monitor saw</h2>
        <p class="note">Time between consecutive requests from the controller. A gap past the deadline line means the watchdog stops the instrument.</p>
        <svg id="gap-chart" width="100%" viewBox="0 0 640 300" role="img" aria-label="Heartbeat gap per cycle"></svg>
        <div class="legend" id="gap-legend"></div>
      </div>
      <div>
        <h2>Where the instrument tip went</h2>
        <p class="note">Every move the monitor approved, in the 8 &times; 8 workspace.</p>
        <svg id="path-chart" width="100%" viewBox="0 0 360 360" role="img" aria-label="Tip path in the workspace"></svg>
        <div class="legend" id="path-legend"></div>
      </div>
    </div>
  </div>

  <div class="card">
    <h2>How late do 10 ms wake-ups run on this QNX target?</h2>
    <p class="note">Every controller wake-up across all runs, measured against its scheduled release. The test margin is how much lateness the timing scenarios tolerate before an outcome could flip.</p>
    <svg id="hist" width="100%" viewBox="0 0 1060 260" role="img" aria-label="Histogram of wake-up lateness"></svg>
    <details><summary>Show all run data as a table</summary><table id="raw"></table></details>
  </div>
</div>
<div class="tip" id="tip"></div>
</div>
<script>
const DATA = __DATA__;
const NS = 'http://www.w3.org/2000/svg';
const css = (v) => getComputedStyle(document.querySelector('.viz-root')).getPropertyValue(v).trim();
const el = (tag, attrs, text) => {
  const e = document.createElementNS(NS, tag);
  for (const k in attrs) e.setAttribute(k, attrs[k]);
  if (text !== undefined) e.textContent = text;
  return e;
};
const tip = document.getElementById('tip');
function showTip(evt, html) {
  tip.innerHTML = html; tip.style.display = 'block';
  const x = Math.min(evt.clientX + 14, window.innerWidth - tip.offsetWidth - 8);
  tip.style.left = x + 'px'; tip.style.top = (evt.clientY + 14) + 'px';
}
function hideTip() { tip.style.display = 'none'; }
const fmt = (v, d = 1) => Number(v).toFixed(d);
const P = DATA.profile;

// ---- header + tiles ---------------------------------------------------------
document.getElementById('meta').textContent =
  `Profile "${DATA.meta.profile}" (period ${P.period} ms, deadline ${P.deadline} ms, test margin ${P.margin} ms) · ` +
  `${DATA.meta.repeats} runs per scenario · ${DATA.meta.uname || ''} · ${DATA.meta.date || ''}`;

const rows = DATA.summary;
const scenarios = [...new Set(rows.map(r => r.scenario))];
const passed = rows.filter(r => r.ok === '1').length;
const lateAll = Object.values(DATA.traces).flatMap(t => t.late);
const worstLate = Math.max(...lateAll);
const worstRtt = Math.max(...rows.map(r => +r.max_rtt_ms));
const tiles = [
  ['Runs passed', `${passed} / ${rows.length}`, passed === rows.length ? 'every run behaved as specified' : 'see the red squares below'],
  ['Scenarios', scenarios.length, 'normal, timing and fault cases'],
  ['Worst wake-up lateness', `${fmt(worstLate)} ms`, `test margin ${P.margin} ms`],
  ['Slowest MsgSend round trip', `${fmt(worstRtt, 2)} ms`, 'controller to monitor and back'],
];
document.getElementById('tiles').innerHTML = tiles.map(([l, v, h]) =>
  `<div class="tile"><div class="label">${l}</div><div class="value">${v}</div><div class="hint">${h}</div></div>`).join('');

// ---- outcome matrix -----------------------------------------------------------
const matrix = document.getElementById('matrix');
function stopText(r) {
  if (r.scenario === 'crash') return r.monitor_watchdog === '1' ? 'watchdog timeout (monitor)' : 'no stop';
  return r.got === 'no stop' ? 'no stop' : `${r.got} at step ${r.stop_step}`;
}
matrix.innerHTML = '<tr><th>Scenario</th><th>Expected</th><th>Runs</th><th>Result (run 1)</th><th>Avg / max wake-up late</th></tr>' +
  scenarios.map(s => {
    const rs = rows.filter(r => r.scenario === s);
    const cells = rs.map(r => r.ok === '1'
      ? `<span class="cell ok" title="run ${r.run}: pass" aria-label="run ${r.run} passed">&#10003;</span>`
      : `<span class="cell bad" title="run ${r.run}: fail" aria-label="run ${r.run} failed">&#10007;</span>`).join('');
    const allOk = rs.every(r => r.ok === '1');
    const avg = rs.reduce((a, r) => a + +r.avg_late_ms, 0) / rs.length;
    const max = Math.max(...rs.map(r => +r.max_late_ms));
    return `<tr class="click" data-s="${s}"><td><b>${s}</b></td><td>${rs[0].expected}</td>` +
      `<td class="runs">${cells} <span class="status ${allOk ? 'ok' : 'bad'}">${rs.filter(r => r.ok === '1').length}/${rs.length}</span></td>` +
      `<td>${stopText(rs[0])}</td><td>${fmt(avg)} / ${fmt(max)} ms</td></tr>`;
  }).join('');
matrix.querySelectorAll('tr.click').forEach(tr => tr.addEventListener('click', () => {
  selScenario.value = tr.dataset.s; fillRuns(); render();
  document.getElementById('sel-scenario').scrollIntoView({ behavior: 'smooth', block: 'center' });
}));

// ---- selection ------------------------------------------------------------------
const selScenario = document.getElementById('sel-scenario');
const selRun = document.getElementById('sel-run');
selScenario.innerHTML = scenarios.map(s => `<option>${s}</option>`).join('');
// Open on the first failed run if there is one (the interesting case),
// otherwise on freeze_past.
const firstFail = rows.find(r => r.ok !== '1');
selScenario.value = firstFail ? firstFail.scenario
  : scenarios.includes('freeze_past') ? 'freeze_past' : scenarios[0];
function fillRuns() {
  const rs = rows.filter(r => r.scenario === selScenario.value);
  selRun.innerHTML = rs.map(r => `<option value="${r.run}">run ${r.run}${r.ok === '1' ? '' : ' (failed)'}</option>`).join('');
}
selScenario.addEventListener('change', () => { fillRuns(); render(); });
selRun.addEventListener('change', render);
fillRuns();
if (firstFail) selRun.value = firstFail.run;

// ---- gap chart ------------------------------------------------------------------
function gapChart(tr, row) {
  const svg = document.getElementById('gap-chart'); svg.innerHTML = '';
  const W = 640, H = 300, L = 48, R = 16, T = 26, B = 34;
  const n = tr.t.length;
  const gaps = tr.t.map((t, i) => i === 0 ? null : t - tr.t[i - 1]);
  const ymax = Math.max(P.deadline * 1.35, ...gaps.filter(g => g !== null)) * 1.18;
  const xs = i => L + (W - L - R) * (n <= 1 ? 0 : i / (n - 1));
  const ys = v => T + (H - T - B) * (1 - v / ymax);
  // grid + y axis
  const step = ymax > 120 ? 40 : ymax > 60 ? 20 : 10;
  for (let v = 0; v <= ymax; v += step) {
    svg.append(el('line', { x1: L, x2: W - R, y1: ys(v), y2: ys(v), stroke: css('--grid') }));
    svg.append(el('text', { x: L - 6, y: ys(v) + 4, 'text-anchor': 'end' }, v));
  }
  svg.append(el('text', { x: L - 6, y: 12, 'text-anchor': 'end' }, 'ms'));
  svg.append(el('text', { x: (L + W - R) / 2, y: H - 6, 'text-anchor': 'middle' }, 'cycle (request number)'));
  // reference lines: period and deadline
  const ref = (v, color, label, dash) => {
    svg.append(el('line', { x1: L, x2: W - R, y1: ys(v), y2: ys(v), stroke: color, 'stroke-width': 1.5, 'stroke-dasharray': dash }));
    svg.append(el('text', { x: L + 6, y: ys(v) - 5, class: 'title-ink' }, label));
  };
  ref(P.period, css('--axis'), `period ${P.period} ms`, '4 4');
  ref(P.deadline, css('--critical'), `deadline ${P.deadline} ms`, '6 4');
  // line
  let d = '';
  gaps.forEach((g, i) => { if (g !== null) d += (d ? 'L' : 'M') + xs(i).toFixed(1) + ',' + ys(g).toFixed(1); });
  svg.append(el('path', { d, fill: 'none', stroke: css('--series-1'), 'stroke-width': 2, 'stroke-linejoin': 'round' }));
  // stop marker (the rejected request is the last trace row)
  const last = n - 1;
  if (n > 0 && tr.ok[last] === 0 && gaps[last] !== null) {
    svg.append(el('circle', { cx: xs(last), cy: ys(gaps[last]), r: 6, fill: css('--critical'), stroke: css('--surface-1'), 'stroke-width': 2 }));
    const right = xs(last) > W - 200;
    const ly = ys(gaps[last]) < T + 24 ? ys(gaps[last]) + 4 : ys(gaps[last]) - 10;
    svg.append(el('text', { x: right ? xs(last) - 12 : xs(last) + 12, y: ly, 'text-anchor': right ? 'end' : 'start', class: 'title-ink' },
      `SAFE_STOP: ${DATA.reasons[tr.reason[last]]}`));
  }
  // hover: crosshair + tooltip
  const cross = el('line', { y1: T, y2: H - B, stroke: css('--axis'), visibility: 'hidden' });
  const dot = el('circle', { r: 4, fill: css('--series-1'), stroke: css('--surface-1'), 'stroke-width': 2, visibility: 'hidden' });
  svg.append(cross, dot);
  const hit = el('rect', { x: L, y: T, width: W - L - R, height: H - T - B, fill: 'transparent' });
  hit.addEventListener('mousemove', (e) => {
    const box = svg.getBoundingClientRect();
    const px = (e.clientX - box.left) * (W / box.width);
    const i = Math.max(1, Math.min(n - 1, Math.round((px - L) / (W - L - R) * (n - 1))));
    if (gaps[i] === null) return;
    cross.setAttribute('x1', xs(i)); cross.setAttribute('x2', xs(i)); cross.setAttribute('visibility', 'visible');
    dot.setAttribute('cx', xs(i)); dot.setAttribute('cy', ys(gaps[i])); dot.setAttribute('visibility', 'visible');
    const verdict = tr.ok[i] ? 'approved' : `<b style="color:${css('--critical')}">refused: ${DATA.reasons[tr.reason[i]]}</b>`;
    showTip(e, `<b>cycle ${i}</b><br>gap ${fmt(gaps[i], 2)} ms<br>wake-up late ${fmt(tr.late[i], 2)} ms<br>MsgSend ${fmt(tr.rtt[i], 2)} ms<br>${verdict}`);
  });
  hit.addEventListener('mouseleave', () => { cross.setAttribute('visibility', 'hidden'); dot.setAttribute('visibility', 'hidden'); hideTip(); });
  svg.append(hit);
  const crashNote = row.scenario === 'crash' ? ' · controller went silent after the last cycle; the monitor\\'s watchdog latched the stop' : '';
  document.getElementById('gap-legend').innerHTML =
    `<span><i style="width:16px;height:2px;background:${css('--series-1')}"></i>heartbeat gap</span>` +
    `<span><i style="width:16px;height:0;border-top:2px dashed ${css('--critical')}"></i>deadline</span>` +
    `<span><i style="width:10px;height:10px;border-radius:50%;background:${css('--critical')}"></i>safe stop</span>` +
    `<span>${n} requests${crashNote}</span>`;
}

// ---- path chart -----------------------------------------------------------------
function pathChart(tr) {
  const svg = document.getElementById('path-chart'); svg.innerHTML = '';
  const S = 360, M = 28, E = DATA.envelope;
  const sc = v => M + (S - 2 * M) * (v - E.min) / (E.max - E.min);
  const sx = sc, sy = v => S - sc(v);
  for (let v = E.min; v <= E.max; v += 1) {
    svg.append(el('line', { x1: sx(v), x2: sx(v), y1: sy(E.min), y2: sy(E.max), stroke: css('--grid') }));
    svg.append(el('line', { y1: sy(v), y2: sy(v), x1: sx(E.min), x2: sx(E.max), stroke: css('--grid') }));
  }
  svg.append(el('rect', { x: sx(E.min), y: sy(E.max), width: sx(E.max) - sx(E.min), height: sy(E.min) - sy(E.max), fill: 'none', stroke: css('--axis'), 'stroke-width': 1.5 }));
  svg.append(el('text', { x: sx(E.min), y: S - 8 }, '0'));
  svg.append(el('text', { x: sx(E.max), y: S - 8, 'text-anchor': 'end' }, '8'));
  for (const z of E.zones) {
    svg.append(el('circle', { cx: sx(z.x), cy: sy(z.y), r: sc(z.r) - sc(0), fill: css('--zone-fill'), stroke: css('--critical'), 'stroke-dasharray': '5 4', 'stroke-width': 1.5 }));
    svg.append(el('text', { x: sx(z.x), y: sy(z.y) + 4, 'text-anchor': 'middle', class: 'title-ink' }, 'no-go zone'));
  }
  const pts = tr.x.map((x, i) => [x, tr.y[i]]).filter((p, i) => tr.ok[i]);
  if (pts.length) {
    const d = pts.map((p, i) => (i ? 'L' : 'M') + sx(p[0]).toFixed(1) + ',' + sy(p[1]).toFixed(1)).join('');
    svg.append(el('path', { d, fill: 'none', stroke: css('--series-1'), 'stroke-width': 2, 'stroke-linejoin': 'round' }));
    svg.append(el('circle', { cx: sx(pts[0][0]), cy: sy(pts[0][1]), r: 5, fill: css('--surface-1'), stroke: css('--series-1'), 'stroke-width': 2 }));
    const end = pts[pts.length - 1];
    const stopped = tr.ok[tr.ok.length - 1] === 0;
    svg.append(el('circle', { cx: sx(end[0]), cy: sy(end[1]), r: 6, fill: stopped ? css('--critical') : css('--series-1'), stroke: css('--surface-1'), 'stroke-width': 2 }));
    const hit = el('rect', { x: 0, y: 0, width: S, height: S, fill: 'transparent' });
    hit.addEventListener('mousemove', (e) => {
      const box = svg.getBoundingClientRect();
      const px = (e.clientX - box.left) * (S / box.width), py = (e.clientY - box.top) * (S / box.height);
      let best = 0, bd = 1e9;
      pts.forEach((p, i) => { const dd = (sx(p[0]) - px) ** 2 + (sy(p[1]) - py) ** 2; if (dd < bd) { bd = dd; best = i; } });
      if (bd > 400) { hideTip(); return; }
      showTip(e, `approved move ${best}<br>tip (${fmt(pts[best][0], 3)}, ${fmt(pts[best][1], 3)})`);
    });
    hit.addEventListener('mouseleave', hideTip);
    svg.append(hit);
  }
  document.getElementById('path-legend').innerHTML =
    `<span><i style="width:10px;height:10px;border-radius:50%;border:2px solid ${css('--series-1')}"></i>start</span>` +
    `<span><i style="width:16px;height:2px;background:${css('--series-1')}"></i>approved path</span>` +
    `<span><i style="width:10px;height:10px;border-radius:50%;background:${css('--critical')}"></i>held here after safe stop</span>`;
}

function render() {
  const key = `${selScenario.value}_${selRun.value}`;
  const tr = DATA.traces[key];
  const row = rows.find(r => r.scenario === selScenario.value && r.run === selRun.value);
  document.querySelectorAll('#matrix tr.click').forEach(t => t.classList.toggle('sel', t.dataset.s === selScenario.value));
  document.getElementById('sel-desc').textContent = row ? `expected: ${row.expected} · got: ${stopText(row)}` : '';
  if (!tr) return;
  gapChart(tr, row);
  pathChart(tr);
}

// ---- lateness histogram ---------------------------------------------------------
function histogram() {
  const svg = document.getElementById('hist'); svg.innerHTML = '';
  const W = 1060, H = 260, L = 56, R = 16, T = 14, B = 36;
  const bin = 0.5;
  const xmax = Math.max(P.margin * 1.1, Math.ceil(worstLate / bin) * bin + bin);
  const nb = Math.ceil(xmax / bin);
  const counts = new Array(nb).fill(0);
  lateAll.forEach(v => { counts[Math.min(nb - 1, Math.max(0, Math.floor(v / bin)))]++; });
  const cmax = Math.max(...counts);
  const xs = v => L + (W - L - R) * v / xmax;
  const ys = c => T + (H - T - B) * (1 - c / cmax);
  const ystep = Math.pow(10, Math.floor(Math.log10(cmax)));
  for (let c = 0; c <= cmax; c += ystep * (cmax / ystep > 5 ? 2 : 1)) {
    svg.append(el('line', { x1: L, x2: W - R, y1: ys(c), y2: ys(c), stroke: css('--grid') }));
    svg.append(el('text', { x: L - 6, y: ys(c) + 4, 'text-anchor': 'end' }, c));
  }
  for (let v = 0; v <= xmax; v += xmax > 30 ? 5 : 2) {
    svg.append(el('text', { x: xs(v), y: H - B + 16, 'text-anchor': 'middle' }, v));
  }
  svg.append(el('text', { x: (L + W - R) / 2, y: H - 4, 'text-anchor': 'middle' }, 'wake-up lateness (ms)'));
  svg.append(el('text', { x: 8, y: T + 4 }, 'wake-ups'));
  const bw = xs(bin) - xs(0);
  counts.forEach((c, i) => {
    if (!c) return;
    const x = xs(i * bin) + 1, h = (H - B) - ys(c);
    const g = el('g');
    const r = el('rect', { x, y: ys(c), width: Math.max(1, bw - 2), height: Math.max(1, h), rx: Math.min(4, (bw - 2) / 2), fill: css('--series-1') });
    const hit = el('rect', { x: xs(i * bin), y: T, width: bw, height: H - T - B, fill: 'transparent' });
    hit.addEventListener('mousemove', (e) => showTip(e, `<b>${fmt(i * bin)}–${fmt((i + 1) * bin)} ms late</b><br>${c.toLocaleString()} wake-ups (${fmt(100 * c / lateAll.length, 1)}%)`));
    hit.addEventListener('mouseleave', hideTip);
    g.append(r, hit); svg.append(g);
  });
  const mark = (v, color, label, dash) => {
    svg.append(el('line', { x1: xs(v), x2: xs(v), y1: T, y2: H - B, stroke: color, 'stroke-width': 1.5, 'stroke-dasharray': dash }));
    svg.append(el('text', { x: xs(v) - 6, y: T + 12, 'text-anchor': 'end', class: 'title-ink' }, label));
  };
  mark(P.margin, css('--critical'), `test margin ${P.margin} ms`, '6 4');
  mark(worstLate, css('--axis'), `worst ${fmt(worstLate)} ms`, '3 3');
  // data table
  document.getElementById('raw').innerHTML =
    '<tr><th>Scenario</th><th>Run</th><th>Pass</th><th>Expected</th><th>Got</th><th>Steps</th><th>Avg late</th><th>Max late</th><th>&gt;2 ms</th><th>Max MsgSend</th></tr>' +
    rows.map(r => `<tr><td>${r.scenario}</td><td>${r.run}</td><td>${r.ok === '1' ? 'yes' : 'NO'}</td><td>${r.expected}</td><td>${stopText(r)}</td><td>${r.steps}</td><td>${fmt(r.avg_late_ms, 2)}</td><td>${fmt(r.max_late_ms, 2)}</td><td>${r.late_over_2ms}</td><td>${fmt(r.max_rtt_ms, 2)}</td></tr>`).join('');
}

render();
histogram();
matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => { render(); histogram(); });
</script>
</body>
</html>
"""


def main():
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    results = sys.argv[1] if len(sys.argv) > 1 else os.path.join(repo, "build", "qnx-results")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(repo, "build", "qnx-report.html")
    if not os.path.exists(os.path.join(results, "summary.csv")):
        sys.exit(f"No results in {results}: run `make qnx-scenarios` then `make qnx-report`.")
    meta, summary, traces = load(results)
    profile = PROFILES.get(meta.get("profile", "qemu"), PROFILES["qemu"])
    data = {"meta": meta, "summary": summary, "traces": traces,
            "profile": profile, "envelope": ENVELOPE,
            "reasons": {str(k): v for k, v in REASONS.items()}}
    # Embed as JSON; escape "</" so the data can never close the script tag.
    blob = json.dumps(data, separators=(",", ":")).replace("</", "<\\/")
    with open(out, "w") as f:
        f.write(PAGE.replace("__DATA__", blob))
    passed = sum(1 for r in summary if r["ok"] == "1")
    print(f"Wrote {out}: {passed}/{len(summary)} runs passed, "
          f"{len(traces)} traces ({html.escape(meta.get('profile', '?'))} profile)")


if __name__ == "__main__":
    main()
