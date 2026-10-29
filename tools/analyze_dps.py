"""Analyse PD2 DPS Meter hit logs and write an HTML report.

    python analyze_dps.py                                  # all logs in ..\\bin\\Release\\logs
    python analyze_dps.py <meter folder>\\logs\\hits_*.csv     # selected files
    python analyze_dps.py logs --gap 5 --min-hits 20 --monsters dummy_monsters.json

Input : hits_*.csv written by PD2DpsHook.dll (format 2: one line per counted hit).
Output: dps_report.html (summary + charts + full data) and dps_report_per_second.csv
        next to the first input file, or where --out says.

Sessions come from the meter (every RESET starts a new one). --gap additionally splits a
session at pauses longer than that many seconds, so several test runs without pressing
RESET still end up separate.

DPS definitions used in the report
    Avg DPS     total damage / active time, active time = last hit - first hit + one median
                hit interval (so N hits count as N intervals, not N-1)
    Peak 1s     highest damage inside any whole second of the session
    Steady DPS  median of the per-second damage, ignoring the first and last second
                (ramp-up / partial seconds) - robust against single big crits
Standard library only (Python 3.13).
"""

import argparse
import csv
import glob
import html
import json
import math
import os
import statistics
import sys
from datetime import datetime

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_LOGS = os.path.join(HERE, '..', 'bin', 'Release', 'logs')


# ======================================================================================
#  Loading
# ======================================================================================
class Hit:
    __slots__ = ('time', 'session', 'damage', 'before', 'after', 'killed', 'unit_type', 'class_id', 'unit_id', 'file')

    def __init__(self, row, file):
        self.time = datetime.strptime(row['time'], '%Y-%m-%d %H:%M:%S.%f')
        self.session = int(row['session'])
        self.damage = float(row['damage'])
        self.before = float(row['life_before'])
        self.after = float(row['life_after'])
        self.killed = row['killed'] == '1'
        self.unit_type = int(row['unit_type'])
        self.class_id = int(row['class_id'])
        self.unit_id = int(row['unit_id'])
        self.file = file


def read_log(path):
    hits = []
    with open(path, newline='', encoding='utf-8', errors='replace') as f:
        lines = [l for l in f if not l.startswith('#')]
    if not lines or not lines[0].startswith('time,'):
        print(f'skipped (not a format 2 hit log): {path}')
        return hits
    for row in csv.DictReader(lines):
        try:
            hits.append(Hit(row, os.path.basename(path)))
        except (KeyError, ValueError):
            continue
    return hits


def collect_files(inputs):
    files = []
    for item in inputs:
        if os.path.isdir(item):
            files += sorted(glob.glob(os.path.join(item, 'hits_*.csv')))
        else:
            files += sorted(glob.glob(item))
    return files


# ======================================================================================
#  Sessions + statistics
# ======================================================================================
class Session:
    def __init__(self, number, hits):
        self.number = number
        self.hits = hits
        self.start = hits[0].time
        t0 = hits[0].time
        self.t = [(h.time - t0).total_seconds() for h in hits]
        self.compute()

    def compute(self):
        hits, t = self.hits, self.t
        dmg = [h.damage for h in hits]
        self.total = sum(dmg)
        self.count = len(hits)
        intervals = [b - a for a, b in zip(t, t[1:]) if b > a]
        self.median_interval = statistics.median(intervals) if intervals else 0.0
        self.duration = (t[-1] - t[0]) + self.median_interval
        self.avg_dps = self.total / self.duration if self.duration > 0 else 0.0
        self.hits_per_s = self.count / self.duration if self.duration > 0 else 0.0
        self.avg_hit = self.total / self.count
        self.median_hit = statistics.median(dmg)
        self.min_hit, self.max_hit = min(dmg), max(dmg)
        self.stdev_hit = statistics.pstdev(dmg) if len(dmg) > 1 else 0.0
        q = sorted(dmg)
        self.p10 = q[int(0.10 * (len(q) - 1))]
        self.p90 = q[int(0.90 * (len(q) - 1))]

        # damage per whole second
        nsec = int(math.floor(t[-1])) + 1
        self.per_second = [0.0] * nsec
        self.hits_per_second = [0] * nsec
        for ti, d in zip(t, dmg):
            self.per_second[int(ti)] += d
            self.hits_per_second[int(ti)] += 1
        self.peak_1s = max(self.per_second)
        inner = self.per_second[1:-1]
        self.steady_dps = statistics.median(inner) if inner else self.avg_dps
        # rolling 5 s average (trailing window, shorter at the start)
        self.rolling5 = []
        for i in range(nsec):
            w = self.per_second[max(0, i - 4):i + 1]
            self.rolling5.append(sum(w) / len(w))

        # kills / time to kill per unit
        first_hit, self.kills = {}, []
        for ti, h in zip(t, hits):
            first_hit.setdefault(h.unit_id, ti)
            if h.killed:
                self.kills.append({'t': ti, 'class_id': h.class_id, 'unit_id': h.unit_id,
                                   'ttk': ti - first_hit.pop(h.unit_id), 'max_life': None})
        self.targets = sorted({h.class_id for h in hits})
        self.files = sorted({h.file for h in hits})


def split_sessions(hits, gap):
    hits.sort(key=lambda h: h.time)
    groups, current, key = [], [], None
    for h in hits:
        k = (h.file, h.session)
        if current and (k != key or (gap and (h.time - current[-1].time).total_seconds() > gap)):
            groups.append(current)
            current = []
        current.append(h)
        key = k
    if current:
        groups.append(current)
    return groups


# ======================================================================================
#  SVG charts (no external libraries; colours via CSS variables -> light & dark mode)
# ======================================================================================
def nice_ceiling(v):
    if v <= 0:
        return 1
    e = 10 ** math.floor(math.log10(v))
    for m in (1, 2, 2.5, 5, 10):
        if v <= m * e:
            return m * e
    return 10 * e


def fmt(v, digits=0):
    return f'{v:,.{digits}f}'


def short(v):
    for div, suf in ((1e6, 'M'), (1e3, 'K')):
        if abs(v) >= div:
            s = f'{v / div:.1f}'.rstrip('0').rstrip('.')
            return s + suf
    return f'{v:.0f}'


def dps_chart(s, idx):
    """Damage per second (bars) + 5 s rolling average (line). One y axis."""
    W, H, L, R, T, B = 760, 260, 56, 64, 16, 34
    n = len(s.per_second)
    ymax = nice_ceiling(max(max(s.per_second), max(s.rolling5)) * 1.05)
    pw, ph = W - L - R, H - T - B
    bw = pw / n
    x = lambda i: L + i * bw
    y = lambda v: T + ph - v / ymax * ph
    out = [f'<svg class="chart" viewBox="0 0 {W} {H}" role="img" aria-label="Damage per second, session {s.number}" '
           f'data-chart="dps{idx}">']
    for k in range(5):   # recessive grid + y labels
        v = ymax * k / 4
        out.append(f'<line class="grid" x1="{L}" x2="{W - R}" y1="{y(v):.1f}" y2="{y(v):.1f}"/>')
        out.append(f'<text class="tick" x="{L - 6}" y="{y(v) + 4:.1f}" text-anchor="end">{short(v)}</text>')
    step = max(1, round(n / 10))
    for i in range(0, n, step):
        out.append(f'<text class="tick" x="{x(i) + bw / 2:.1f}" y="{H - B + 16}" text-anchor="middle">{i}s</text>')
    gap = 2 if bw > 6 else 0
    for i, v in enumerate(s.per_second):
        if v <= 0:
            continue
        h = max(1.0, ph * v / ymax)
        w = max(1.0, bw - gap)
        r = min(4, w / 2)
        x0, y0 = x(i) + gap / 2, y(v)
        # rounded data end at the top, square at the baseline
        out.append(f'<path class="bar s1" d="M{x0:.1f},{T + ph:.1f} V{y0 + r:.1f} Q{x0:.1f},{y0:.1f} {x0 + r:.1f},{y0:.1f} '
                   f'H{x0 + w - r:.1f} Q{x0 + w:.1f},{y0:.1f} {x0 + w:.1f},{y0 + r:.1f} V{T + ph:.1f} Z"/>')
    pts = ' '.join(f'{x(i) + bw / 2:.1f},{y(v):.1f}' for i, v in enumerate(s.rolling5))
    out.append(f'<polyline class="line s2" points="{pts}"/>')
    # direct label at the line end
    out.append(f'<text class="direct" x="{W - R + 6}" y="{y(s.rolling5[-1]) + 4:.1f}">5s avg</text>')
    out.append(f'<line class="axis" x1="{L}" x2="{W - R}" y1="{T + ph}" y2="{T + ph}"/>')
    # hover layer: one invisible column per second
    for i in range(n):
        tip = (f'Second {i}: {fmt(s.per_second[i])} damage, {s.hits_per_second[i]} hits'
               f' | 5s avg {fmt(s.rolling5[i])}')
        out.append(f'<rect class="hit" x="{x(i):.1f}" y="{T}" width="{bw:.1f}" height="{ph}" data-tip="{html.escape(tip)}"/>')
    out.append('</svg>')
    return '\n'.join(out)


def histogram(s, idx):
    """Distribution of single hit damage (one series)."""
    W, H, L, R, T, B = 760, 200, 56, 16, 12, 34
    dmg = [h.damage for h in s.hits]
    lo, hi = min(dmg), max(dmg)
    nb = max(5, min(30, int(math.sqrt(len(dmg))) + 1))
    if hi == lo:
        hi = lo + 1
    width = (hi - lo) / nb
    counts = [0] * nb
    for d in dmg:
        counts[min(nb - 1, int((d - lo) / width))] += 1
    cmax = nice_ceiling(max(counts))
    pw, ph = W - L - R, H - T - B
    bw = pw / nb
    out = [f'<svg class="chart" viewBox="0 0 {W} {H}" role="img" aria-label="Hit damage distribution, session {s.number}">']
    for k in range(3):
        v = cmax * k / 2
        yy = T + ph - v / cmax * ph
        out.append(f'<line class="grid" x1="{L}" x2="{W - R}" y1="{yy:.1f}" y2="{yy:.1f}"/>')
        out.append(f'<text class="tick" x="{L - 6}" y="{yy + 4:.1f}" text-anchor="end">{v:.0f}</text>')
    for i, c in enumerate(counts):
        x0 = L + i * bw + 1
        if c:
            h = ph * c / cmax
            y0 = T + ph - h
            w = bw - 2
            r = min(4, w / 2, h)
            out.append(f'<path class="bar s1" d="M{x0:.1f},{T + ph:.1f} V{y0 + r:.1f} Q{x0:.1f},{y0:.1f} {x0 + r:.1f},{y0:.1f} '
                       f'H{x0 + w - r:.1f} Q{x0 + w:.1f},{y0:.1f} {x0 + w:.1f},{y0 + r:.1f} V{T + ph:.1f} Z"/>')
        tip = f'{fmt(lo + i * width)} - {fmt(lo + (i + 1) * width)} damage: {c} hits'
        out.append(f'<rect class="hit" x="{L + i * bw:.1f}" y="{T}" width="{bw:.1f}" height="{ph}" data-tip="{html.escape(tip)}"/>')
    for i in (0, nb // 2, nb):
        out.append(f'<text class="tick" x="{L + i * bw:.1f}" y="{H - B + 16}" text-anchor="middle">{short(lo + i * width)}</text>')
    out.append(f'<line class="axis" x1="{L}" x2="{W - R}" y1="{T + ph}" y2="{T + ph}"/>')
    out.append('</svg>')
    return '\n'.join(out)


def comparison_chart(sessions):
    """Avg DPS per session (horizontal bars, one series)."""
    W, rowh, L, R, T = 760, 26, 120, 80, 8
    H = T * 2 + rowh * len(sessions)
    xmax = nice_ceiling(max(s.avg_dps for s in sessions))
    pw = W - L - R
    out = [f'<svg class="chart" viewBox="0 0 {W} {H}" role="img" aria-label="Average DPS per session">']
    for i, s in enumerate(sessions):
        y0 = T + i * rowh + 4
        h = rowh - 8
        w = max(2.0, pw * s.avg_dps / xmax)
        r = min(4, h / 2, w)
        out.append(f'<text class="tick" x="{L - 8}" y="{y0 + h / 2 + 4:.1f}" text-anchor="end">#{s.number} {s.start:%H:%M:%S}</text>')
        out.append(f'<path class="bar s1" d="M{L},{y0} H{L + w - r:.1f} Q{L + w:.1f},{y0} {L + w:.1f},{y0 + r:.1f} '
                   f'V{y0 + h - r:.1f} Q{L + w:.1f},{y0 + h} {L + w - r:.1f},{y0 + h} H{L} Z"/>')
        out.append(f'<text class="value" x="{L + w + 6:.1f}" y="{y0 + h / 2 + 4:.1f}">{fmt(s.avg_dps)}</text>')
        tip = f'Session {s.number}: avg {fmt(s.avg_dps)} DPS, peak 1s {fmt(s.peak_1s)}, {s.count} hits, {s.duration:.1f} s'
        out.append(f'<rect class="hit" x="{L}" y="{y0 - 3}" width="{pw}" height="{rowh}" data-tip="{html.escape(tip)}"/>')
    out.append(f'<line class="axis" x1="{L}" x2="{L}" y1="{T}" y2="{H - T}"/>')
    out.append('</svg>')
    return '\n'.join(out)


# ======================================================================================
#  HTML
# ======================================================================================
CSS = """
:root { color-scheme: light;
  --surface: #fcfcfb; --surface-2: #f3f2ef; --border: #e2e1dc;
  --text-1: #0b0b0b; --text-2: #52514e; --text-3: #7a7974;
  --s1: #2a78d6; --s2: #eb6834; --grid: #e8e7e3; --axis: #b9b8b2; }
@media (prefers-color-scheme: dark) { :root:where(:not([data-theme="light"])) { color-scheme: dark;
  --surface: #1a1a19; --surface-2: #232321; --border: #383835;
  --text-1: #f0efec; --text-2: #c3c2b7; --text-3: #92918a;
  --s1: #3987e5; --s2: #d95926; --grid: #2c2c2a; --axis: #55544f; } }
:root[data-theme="dark"] { color-scheme: dark;
  --surface: #1a1a19; --surface-2: #232321; --border: #383835;
  --text-1: #f0efec; --text-2: #c3c2b7; --text-3: #92918a;
  --s1: #3987e5; --s2: #d95926; --grid: #2c2c2a; --axis: #55544f; }
* { box-sizing: border-box; }
body { margin: 0; background: var(--surface); color: var(--text-1);
  font: 14px/1.45 system-ui, -apple-system, "Segoe UI", sans-serif; }
main { max-width: 1100px; margin: 0 auto; padding: 24px 16px 64px; }
h1 { font-size: 24px; margin: 0 0 4px; } h2 { font-size: 18px; margin: 32px 0 8px; }
h3 { font-size: 15px; margin: 20px 0 6px; color: var(--text-2); }
.meta { color: var(--text-3); font-size: 13px; }
.tiles { display: grid; grid-template-columns: repeat(auto-fit, minmax(150px, 1fr)); gap: 8px; margin: 12px 0; }
.tile { background: var(--surface-2); border: 1px solid var(--border); border-radius: 8px; padding: 10px 12px; }
.tile .label { color: var(--text-2); font-size: 12px; }
.tile .big { font-size: 22px; font-weight: 650; font-variant-numeric: tabular-nums; }
.table-wrap { overflow-x: auto; }
table { border-collapse: collapse; width: 100%; font-variant-numeric: tabular-nums; font-size: 13px; }
th, td { padding: 4px 8px; border-bottom: 1px solid var(--border); text-align: right; white-space: nowrap; }
th { color: var(--text-2); font-weight: 600; position: sticky; top: 0; background: var(--surface); }
th:first-child, td:first-child { text-align: left; }
tr.best td { font-weight: 650; }
.chart { width: 100%; height: auto; display: block; }
.chart .grid { stroke: var(--grid); stroke-width: 1; }
.chart .axis { stroke: var(--axis); stroke-width: 1; }
.chart .tick { fill: var(--text-3); font-size: 11px; }
.chart .value, .chart .direct { fill: var(--text-2); font-size: 12px; }
.chart .bar.s1 { fill: var(--s1); }
.chart .line.s2 { fill: none; stroke: var(--s2); stroke-width: 2; stroke-linejoin: round; }
.chart .hit { fill: transparent; }
.chart .hit:hover { fill: var(--text-1); fill-opacity: .06; }
.legend { display: flex; gap: 16px; color: var(--text-2); font-size: 12px; margin: 4px 0; }
.key { display: inline-block; width: 12px; height: 12px; border-radius: 3px; vertical-align: -2px; margin-right: 4px; }
.key.line { height: 3px; border-radius: 2px; vertical-align: 3px; }
details { margin: 8px 0; } summary { cursor: pointer; color: var(--text-2); }
.raw { max-height: 420px; overflow: auto; border: 1px solid var(--border); border-radius: 6px; }
#tip { position: fixed; pointer-events: none; background: var(--text-1); color: var(--surface);
  padding: 4px 8px; border-radius: 4px; font-size: 12px; display: none; z-index: 10; max-width: 320px; }
.note { color: var(--text-2); font-size: 13px; }
"""

JS = """
const tip = document.getElementById('tip');
document.addEventListener('mousemove', e => {
  const t = e.target.closest('[data-tip]');
  if (!t) { tip.style.display = 'none'; return; }
  tip.textContent = t.dataset.tip; tip.style.display = 'block';
  const x = Math.min(e.clientX + 12, window.innerWidth - tip.offsetWidth - 8);
  tip.style.left = x + 'px'; tip.style.top = (e.clientY + 14) + 'px';
});
"""


def target_name(cid, monsters):
    m = monsters.get(str(cid))
    return f"{m['area']} ({cid})" if m else f'monster {cid}'


def tile(label, value):
    return f'<div class="tile"><div class="label">{label}</div><div class="big">{value}</div></div>'


def build_html(sessions, files, monsters, args):
    best = max(sessions, key=lambda s: s.avg_dps)
    rows = []
    for s in sessions:
        targets = ', '.join(target_name(c, monsters) for c in s.targets[:3]) + (' ...' if len(s.targets) > 3 else '')
        ttk = statistics.mean(k['ttk'] for k in s.kills) if s.kills else None
        rows.append(f'<tr class="{"best" if s is best and len(sessions) > 1 else ""}">'
                    f'<td><a href="#s{s.number}">#{s.number}</a> {s.start:%Y-%m-%d %H:%M:%S}</td>'
                    f'<td>{s.duration:.1f}</td><td>{fmt(s.total)}</td><td>{fmt(s.avg_dps)}</td><td>{fmt(s.steady_dps)}</td>'
                    f'<td>{fmt(s.peak_1s)}</td><td>{s.count}</td><td>{s.hits_per_s:.2f}</td><td>{fmt(s.avg_hit)}</td>'
                    f'<td>{fmt(s.max_hit)}</td><td>{len(s.kills)}</td><td>{"" if ttk is None else f"{ttk:.2f}"}</td>'
                    f'<td style="text-align:left">{html.escape(targets)}</td></tr>')

    parts = [f'<!doctype html><html lang="en"><head><meta charset="utf-8">'
             f'<meta name="viewport" content="width=device-width, initial-scale=1"><title>DPS Test Report</title>'
             f'<style>{CSS}</style></head><body><main>',
             '<h1>DPS test report</h1>',
             f'<div class="meta">{len(sessions)} session(s), {sum(s.count for s in sessions):,} hits from {len(files)} log file(s) '
             f'&middot; generated {datetime.now():%Y-%m-%d %H:%M}'
             + (f' &middot; split at pauses &gt; {args.gap:g} s' if args.gap else '') + '</div>',
             '<div class="tiles">' + tile('Best avg DPS', fmt(best.avg_dps) + f' <span class="meta">#{best.number}</span>')
             + tile('Best peak 1s', fmt(max(s.peak_1s for s in sessions)))
             + tile('Total damage', short(sum(s.total for s in sessions)))
             + tile('Kills', str(sum(len(s.kills) for s in sessions))) + '</div>',
             '<h2>Sessions</h2>',
             '<div class="table-wrap"><table><thead><tr><th>Session</th><th>Time s</th><th>Total</th><th>Avg DPS</th>'
             '<th>Steady DPS</th><th>Peak 1s</th><th>Hits</th><th>Hits/s</th><th>Avg hit</th><th>Max hit</th>'
             '<th>Kills</th><th>Avg TTK s</th><th>Targets</th></tr></thead><tbody>' + ''.join(rows) + '</tbody></table></div>',
             '<p class="note">Avg DPS = total / (last hit - first hit + one median hit interval). Steady DPS = median damage '
             'per second without the first and last second. Peak 1s = best single second.</p>']
    if len(sessions) > 1:
        parts += ['<h3>Average DPS per session</h3>', comparison_chart(sessions)]

    for i, s in enumerate(sessions):
        parts.append(f'<h2 id="s{s.number}">Session #{s.number} &middot; {s.start:%Y-%m-%d %H:%M:%S}</h2>')
        parts.append(f'<div class="meta">{", ".join(s.files)} &middot; targets: '
                     f'{html.escape(", ".join(target_name(c, monsters) for c in s.targets))}</div>')
        parts.append('<div class="tiles">' + tile('Avg DPS', fmt(s.avg_dps)) + tile('Steady DPS', fmt(s.steady_dps))
                     + tile('Peak 1s', fmt(s.peak_1s)) + tile('Duration', f'{s.duration:.1f} s')
                     + tile('Hits / s', f'{s.hits_per_s:.2f}') + tile('Avg / median hit', f'{fmt(s.avg_hit)} / {fmt(s.median_hit)}')
                     + '</div>')
        parts.append('<h3>Damage per second</h3><div class="legend"><span><span class="key" style="background:var(--s1)"></span>'
                     'damage in that second</span><span><span class="key line" style="background:var(--s2)"></span>'
                     '5 s rolling average</span></div>' + dps_chart(s, i))
        parts.append(f'<h3>Hit damage distribution</h3><div class="note">min {fmt(s.min_hit)} &middot; 10% {fmt(s.p10)} '
                     f'&middot; median {fmt(s.median_hit)} &middot; 90% {fmt(s.p90)} &middot; max {fmt(s.max_hit)} '
                     f'&middot; std dev {fmt(s.stdev_hit)}</div>' + histogram(s, i))
        if s.kills:
            krows = ''.join(f'<tr><td>{k["t"]:.2f}</td><td>{html.escape(target_name(k["class_id"], monsters))}</td>'
                            f'<td>{k["unit_id"]}</td><td>{k["ttk"]:.2f}</td></tr>' for k in s.kills)
            parts.append('<h3>Kills</h3><div class="table-wrap"><table><thead><tr><th>At s</th><th>Target</th><th>Unit id</th>'
                         f'<th>Time to kill s</th></tr></thead><tbody>{krows}</tbody></table></div>')
        sec_rows = ''.join(f'<tr><td>{j}</td><td>{fmt(v)}</td><td>{s.hits_per_second[j]}</td><td>{fmt(s.rolling5[j])}</td></tr>'
                           for j, v in enumerate(s.per_second))
        parts.append(f'<details><summary>Per-second data ({len(s.per_second)} rows)</summary><div class="raw"><table>'
                     f'<thead><tr><th>Second</th><th>Damage</th><th>Hits</th><th>5 s avg</th></tr></thead><tbody>{sec_rows}'
                     '</tbody></table></div></details>')
        limit = args.max_rows
        hit_rows = ''.join(f'<tr><td>{h.time:%H:%M:%S.%f}</td><td>{t:.3f}</td><td>{h.damage:,.1f}</td><td>{h.before:,.0f}</td>'
                           f'<td>{h.after:,.0f}</td><td>{"yes" if h.killed else ""}</td><td>{h.class_id}</td><td>{h.unit_id}</td></tr>'
                           for h, t in list(zip(s.hits, s.t))[:limit])
        more = f' (first {limit} of {s.count})' if s.count > limit else ''
        parts.append(f'<details><summary>All hits{more}</summary><div class="raw"><table><thead><tr><th>Time</th><th>t s</th>'
                     f'<th>Damage</th><th>Life before</th><th>Life after</th><th>Kill</th><th>Class</th><th>Unit</th></tr></thead>'
                     f'<tbody>{hit_rows}</tbody></table></div></details>')

    parts.append(f'<div id="tip"></div><script>{JS}</script></main></body></html>')
    return '\n'.join(parts)


def write_per_second_csv(sessions, path):
    with open(path, 'w', newline='', encoding='utf-8') as f:
        w = csv.writer(f)
        w.writerow(['session', 'start', 'second', 'damage', 'hits', 'rolling_5s'])
        for s in sessions:
            for j, v in enumerate(s.per_second):
                w.writerow([s.number, s.start.isoformat(sep=' '), j, round(v, 3), s.hits_per_second[j], round(s.rolling5[j], 3)])


# ======================================================================================
def main():
    ap = argparse.ArgumentParser(description='Analyse PD2 DPS Meter hit logs (hits_*.csv) into an HTML report.')
    ap.add_argument('inputs', nargs='*', default=[DEFAULT_LOGS], help='log files, wildcards or folders')
    ap.add_argument('--out', help='output HTML file (default: dps_report.html next to the logs)')
    ap.add_argument('--gap', type=float, default=0, help='also split sessions at pauses longer than this (s)')
    ap.add_argument('--min-hits', type=int, default=5, help='ignore sessions with fewer hits')
    ap.add_argument('--monsters', help='dummy_monsters.json from the dummy build (names for monster ids)')
    ap.add_argument('--class-ids', help='only count these monster ids, e.g. 1285-1292,1300')
    ap.add_argument('--max-rows', type=int, default=5000, help='max hit rows per session in the raw table')
    args = ap.parse_args()

    files = collect_files(args.inputs)
    if not files:
        sys.exit(f'no hit logs found in: {", ".join(args.inputs)}')
    hits = [h for f in files for h in read_log(f)]

    if args.class_ids:
        allowed = set()
        for part in args.class_ids.split(','):
            a, _, b = part.strip().partition('-')
            allowed.update(range(int(a), int(b or a) + 1))
        hits = [h for h in hits if h.class_id in allowed]

    groups = [g for g in split_sessions(hits, args.gap) if len(g) >= args.min_hits]
    if not groups:
        sys.exit('no session with enough hits (see --min-hits)')
    sessions = [Session(i + 1, g) for i, g in enumerate(groups)]

    monsters = {}
    if args.monsters:
        with open(args.monsters, encoding='utf-8') as f:
            monsters = json.load(f)

    out = args.out or os.path.join(os.path.dirname(os.path.abspath(files[0])), 'dps_report.html')
    with open(out, 'w', encoding='utf-8') as f:
        f.write(build_html(sessions, files, monsters, args))
    csv_out = os.path.splitext(out)[0] + '_per_second.csv'
    write_per_second_csv(sessions, csv_out)

    print(f'{len(sessions)} session(s) from {len(files)} file(s)')
    for s in sessions:
        print(f'  #{s.number} {s.start:%H:%M:%S}  {s.duration:6.1f} s  avg {s.avg_dps:10,.0f} DPS  '
              f'steady {s.steady_dps:10,.0f}  peak {s.peak_1s:10,.0f}  hits {s.count}  kills {len(s.kills)}')
    print(f'report : {out}\ncsv    : {csv_out}')


if __name__ == '__main__':
    main()
