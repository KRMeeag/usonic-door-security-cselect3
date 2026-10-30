#!/usr/bin/env python3
"""
Analyse captured serial logs from the door intrusion detector.

Reads one or more `pio device monitor` capture files, pulls out the machine
readable `#D` (sample) and `#E` (event) lines the firmware interleaves with its
human-readable output, and produces:

  * a printed summary  — detection rate, false positives, latency, dip durations
  * tidy CSVs          — data/samples.csv, data/events.csv, data/trials.csv
  * figures            — docs/figures/*.png, ready to drop into the paper

Capture a run:
    pio device monitor -f log2file          # writes into logs/
    # or
    pio device monitor | tee logs/run01.log

Mark ground truth while testing (see firmware serial console):
    press 1  immediately before each door-opening trial   -> expect an alarm
    press 0  immediately before each disturbance trial     -> expect NO alarm

Then:
    python3 tools/analyze.py logs/*.log

Only matplotlib is optional; parsing and statistics are stdlib.
"""

from __future__ import annotations

import argparse
import csv
import glob
import os
import re
import statistics as stats
import sys

# --------------------------------------------------------------------------
# Palette — validated for all-pairs CVD separation on the light print surface.
# Three sensors map to the first three categorical slots; status colours are
# reserved and always ship alongside a text label, never colour alone.
# --------------------------------------------------------------------------
SERIES = ["#2a78d6", "#eb6834", "#1baf7a"]      # sensor 1 / 2 / 3
GOOD, CRITICAL, WARNING = "#0ca30c", "#d03b3b", "#fab219"
SURFACE, INK, INK_2, MUTED = "#fcfcfb", "#0b0b0b", "#52514e", "#898781"
GRID, BASELINE = "#e1e0d9", "#c3c2b7"

# `monitor_filters = time` prefixes every line; slice from the record marker.
REC = re.compile(r"(#[DEH],.*)$")

SAMPLE_FIELDS = [
    "ms", "state", "armed", "alarm", "d1", "d2", "d3", "hmask", "omask",
    "tmask", "thr", "streak", "risk", "snd", "sndev", "temp", "hum",
    "dhtok", "rssi", "heap",
]
NUMERIC = set(SAMPLE_FIELDS) - {"state"}


# ==========================================================================
# Parsing
# ==========================================================================

def _num(v):
    try:
        return float(v)
    except ValueError:
        return float("nan")


def parse(paths):
    """Return a list of sessions. A session is one boot-to-boot run.

    millis() restarts at 0 on every reset, so a sample timestamp going
    backwards marks a reboot and starts a new session. Keeping sessions
    separate is what stops a power-cycle mid-test from scrambling the
    timeline.
    """
    sessions, cur = [], None

    def new_session(src):
        nonlocal cur
        cur = {"source": src, "samples": [], "events": []}
        sessions.append(cur)

    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for raw in fh:
                m = REC.search(raw)
                if not m:
                    continue
                parts = m.group(1).rstrip("\r\n").split(",")
                kind = parts[0]

                if kind == "#D":
                    vals = parts[1:]
                    if len(vals) < len(SAMPLE_FIELDS):
                        continue                       # truncated line, skip
                    row = dict(zip(SAMPLE_FIELDS, vals))
                    for k in NUMERIC:
                        row[k] = _num(row[k])
                    if cur is None or (cur["samples"] and
                                       row["ms"] < cur["samples"][-1]["ms"]):
                        new_session(os.path.basename(path))
                    cur["samples"].append(row)

                elif kind == "#E":
                    if len(parts) < 3:
                        continue
                    ev = {
                        "ms": _num(parts[1]),
                        "type": parts[2],
                        "detail": ",".join(parts[3:]),
                    }
                    if cur is None:
                        new_session(os.path.basename(path))
                    # A boot event is the other reliable session boundary.
                    if ev["type"] == "boot" and cur["samples"]:
                        new_session(os.path.basename(path))
                    cur["events"].append(ev)

    return [s for s in sessions if s["samples"] or s["events"]]


def cycle_ms(samples):
    """Nominal sampling period, measured rather than assumed."""
    if len(samples) < 3:
        return 300.0
    diffs = [b["ms"] - a["ms"] for a, b in zip(samples, samples[1:])]
    diffs = [d for d in diffs if 0 < d < 5000]
    return stats.median(diffs) if diffs else 300.0


# ==========================================================================
# Trial pairing — builds the confusion matrix with no hand labelling
# ==========================================================================

def build_trials(session, window_ms):
    """Pair each ground-truth marker with what the system actually did.

    A trial runs from its marker until `window_ms` later or the next marker,
    whichever comes first. `1` markers expect an alarm, `0` markers expect
    none, which gives TP / FN / FP / TN directly.
    """
    markers = [e for e in session["events"]
               if e["type"] in ("trial_pos", "trial_neg")]
    alarms = [e for e in session["events"] if e["type"] == "alarm"]
    samples = session["samples"]
    cyc = cycle_ms(samples)
    out = []

    for i, mk in enumerate(markers):
        start = mk["ms"]
        end = start + window_ms
        if i + 1 < len(markers):
            end = min(end, markers[i + 1]["ms"])

        expect = mk["type"] == "trial_pos"
        hit = next((a for a in alarms if start <= a["ms"] <= end), None)
        win = [s for s in samples if start <= s["ms"] <= end]

        # First instant any sensor read open — the device-side start of the
        # event, which is the honest zero point for detection latency.
        first_open = next((s["ms"] for s in win if s["omask"] > 0), None)

        trial = {
            "n": i + 1,
            "marker_ms": start,
            # Trial class rides in the marker's detail field: fast / held / neg.
            # Sweep speed is the independent variable, so everything downstream
            # is reported per class as well as pooled.
            "cls": mk["detail"].strip() or ("pos" if expect else "neg"),
            # tmask bits 3/4 record what each rule WOULD have done on its own,
            # evaluated in parallel for the whole arm cycle. This is what makes
            # the old-vs-new comparison come out of a single run.
            "old_fired": 1 if any(int(s_["tmask"]) & 0x08 for s_ in win) else 0,
            "win_fired": 1 if any(int(s_["tmask"]) & 0x10 for s_ in win) else 0,
            "risk_max": max((s_["risk"] for s_ in win), default=""),
            "expected": "alarm" if expect else "no_alarm",
            "observed": "alarm" if hit else "no_alarm",
            "outcome": ("TP" if expect and hit else
                        "FN" if expect and not hit else
                        "FP" if hit else "TN"),
            "alarm_ms": hit["ms"] if hit else "",
            "first_open_ms": first_open if first_open is not None else "",
            # Device-side: first beam break -> siren. Excludes human reaction.
            "latency_detect_ms": (hit["ms"] - first_open
                                  if hit and first_open is not None else ""),
            # Marker -> siren. Includes whoever was opening the door.
            "latency_marker_ms": hit["ms"] - start if hit else "",
            "min_dist_cm": (min((min(s["d1"], s["d2"], s["d3"]) for s in win),
                                default="") if win else ""),
        }

        # Per-sensor dip spans: how long each beam actually read open. This is
        # what sizes the Tier 3 coincidence window.
        for si in range(3):
            runs, run = [], []
            for s in win:
                if int(s["omask"]) & (1 << si):
                    run.append(s["ms"])
                elif run:
                    runs.append(run)
                    run = []
            if run:
                runs.append(run)
            longest = max(runs, key=len) if runs else []
            trial[f"dip{si + 1}_n"] = len(longest)
            # A 1-sample dip means "at or below the sampling period" — the
            # measurement floor, not a real duration.
            trial[f"dip{si + 1}_ms"] = (longest[-1] - longest[0] + cyc
                                        if longest else 0.0)
        out.append(trial)

    return out


def confusion(trials):
    c = {"TP": 0, "FP": 0, "TN": 0, "FN": 0}
    for t in trials:
        c[t["outcome"]] += 1
    return c


def rates(c):
    tp, fp, tn, fn = c["TP"], c["FP"], c["TN"], c["FN"]
    def div(a, b):
        return a / b if b else float("nan")
    return {
        "precision": div(tp, tp + fp),
        "recall": div(tp, tp + fn),
        "specificity": div(tn, tn + fp),
        "accuracy": div(tp + tn, tp + fp + tn + fn),
        "f1": div(2 * tp, 2 * tp + fp + fn),
        "false_positive_rate": div(fp, fp + tn),
    }


# ==========================================================================
# Reporting
# ==========================================================================

def describe(label, values, unit=""):
    vals = [v for v in values if isinstance(v, (int, float))
            and v == v]  # drop NaN
    if not vals:
        return f"  {label:<26} no data"
    body = (f"n={len(vals):<4} mean={stats.mean(vals):8.1f}{unit}"
            f"  sd={(stats.stdev(vals) if len(vals) > 1 else 0.0):7.1f}{unit}"
            f"  min={min(vals):7.1f}{unit}  max={max(vals):7.1f}{unit}")
    return f"  {label:<26} {body}"


def report(sessions, trials, out=sys.stdout):
    w = out.write
    samples = [s for ses in sessions for s in ses["samples"]]
    events = [e for ses in sessions for e in ses["events"]]

    w("\n" + "=" * 78 + "\n")
    w("DOOR INTRUSION DETECTOR — CAPTURE ANALYSIS\n")
    w("=" * 78 + "\n\n")

    w(f"  sessions (boots)           {len(sessions)}\n")
    w(f"  samples                    {len(samples)}\n")
    w(f"  events                     {len(events)}\n")
    if samples:
        span = sum(ses['samples'][-1]['ms'] - ses['samples'][0]['ms']
                   for ses in sessions if ses['samples']) / 1000.0
        w(f"  total captured             {span / 60:.1f} min\n")
        w(f"  sampling period            {cycle_ms(samples):.0f} ms\n")

    by_type = {}
    for e in events:
        by_type[e["type"]] = by_type.get(e["type"], 0) + 1
    if by_type:
        w("\n  event counts\n")
        for k in sorted(by_type):
            w(f"    {k:<22} {by_type[k]}\n")

    # ---- detection performance -------------------------------------------
    w("\n" + "-" * 78 + "\n")
    w("DETECTION PERFORMANCE  (Req 6)\n")
    w("-" * 78 + "\n")
    if not trials:
        w("\n  No ground-truth markers found.\n"
          "  During a test run, press 1 before each door-opening trial and\n"
          "  0 before each disturbance trial. Without markers the confusion\n"
          "  matrix cannot be built.\n")
    else:
        c = confusion(trials)
        r = rates(c)
        w(f"\n  trials                     {len(trials)}"
          f"   (positive {c['TP'] + c['FN']}, negative {c['FP'] + c['TN']})\n\n")
        w("                        observed\n")
        w("                   alarm    no alarm\n")
        w(f"    expected alarm   {c['TP']:>5}    {c['FN']:>8}   <- TP / FN\n")
        w(f"          no alarm   {c['FP']:>5}    {c['TN']:>8}   <- FP / TN\n\n")
        for k in ("recall", "precision", "specificity",
                  "false_positive_rate", "accuracy", "f1"):
            v = r[k]
            w(f"    {k:<22} {v * 100:6.1f}%\n" if v == v
              else f"    {k:<22}    n/a\n")

        # Per-class breakdown. This is the comparison that carries Tier 3:
        # a held panel is the easy case the midterm rule already passes, a fast
        # sweep is the one it can miss.
        classes = []
        for t in trials:
            if t["cls"] not in classes:
                classes.append(t["cls"])
        if len(classes) > 1:
            w("\n  by trial class\n")
            for cl in classes:
                grp = [t for t in trials if t["cls"] == cl]
                pos = [t for t in grp if t["expected"] == "alarm"]
                if pos:
                    hit = sum(1 for t in pos if t["outcome"] == "TP")
                    w(f"    {cl:<10} n={len(pos):<4} detected"
                      f" {hit}/{len(pos)}   {100.0 * hit / len(pos):5.1f}%\n")
                else:
                    fp = sum(1 for t in grp if t["outcome"] == "FP")
                    w(f"    {cl:<10} n={len(grp):<4} false alarms"
                      f" {fp}/{len(grp)}   {100.0 * fp / len(grp):5.1f}%\n")

        # ---- the Tier 3 result: both rules, one run ----------------------
        has_rules = any(t["old_fired"] or t["win_fired"] for t in trials)
        if has_rules:
            pos = [t for t in trials if t["expected"] == "alarm"]
            neg = [t for t in trials if t["expected"] != "alarm"]
            w("\n  rule comparison  (both evaluated in parallel, same run)\n")
            if pos:
                a = sum(t["old_fired"] for t in pos)
                b = sum(1 for t in pos if t["old_fired"] or t["win_fired"])
                w(f"    persistence alone      {a}/{len(pos)}"
                  f"   {100.0 * a / len(pos):5.1f}%\n")
                w(f"    + coincidence window   {b}/{len(pos)}"
                  f"   {100.0 * b / len(pos):5.1f}%\n")
                w(f"    detections gained      {b - a}\n")
                if b == a:
                    w("    -> no gap on this rig: the window caught nothing the\n"
                      "       persistence rule missed. Report that honestly; it is a\n"
                      "       real negative result, not a failed experiment.\n")
            if neg:
                fa = sum(t["old_fired"] for t in neg)
                fb = sum(1 for t in neg if t["old_fired"] or t["win_fired"])
                w(f"    false alarms           persistence {fa}/{len(neg)},"
                  f" with window {fb}/{len(neg)}\n")
                if fb > fa:
                    w(f"    -> the window costs {fb - fa} extra false alarm(s).\n"
                      f"       Shorten it (serial W<ms>) and re-run the negatives.\n")

        # ---- risk score separation (Req 7 evidence) ----------------------
        rp = [t["risk_max"] for t in trials if t["expected"] == "alarm"
              and isinstance(t["risk_max"], (int, float))]
        rn = [t["risk_max"] for t in trials if t["expected"] != "alarm"
              and isinstance(t["risk_max"], (int, float))]
        if rp and rn:
            w("\n  risk score  (shadow — reported, never triggers)\n")
            w(f"    openings     n={len(rp):<4} mean={stats.mean(rp):5.1f}"
              f"  min={min(rp):5.1f}\n")
            w(f"    disturbances n={len(rn):<4} mean={stats.mean(rn):5.1f}"
              f"  max={max(rn):5.1f}\n")
            gap = min(rp) - max(rn)
            w(f"    separation   {gap:+.1f}"
              f"  ({'clean — no overlap' if gap > 0 else 'OVERLAPPING'})\n")

        w("\n  latency\n")
        w(describe("detect (beam->siren)",
                   [t["latency_detect_ms"] for t in trials], " ms") + "\n")
        w(describe("marker->siren",
                   [t["latency_marker_ms"] for t in trials], " ms") + "\n")

    # ---- dip durations ---------------------------------------------------
    w("\n" + "-" * 78 + "\n")
    w("BEAM DIP DURATIONS  —  sizes the Tier 3 coincidence window\n")
    w("-" * 78 + "\n\n")
    pos = [t for t in trials if t["expected"] == "alarm"]
    if not pos:
        w("  No positive trials; nothing to measure.\n")
    else:
        cyc = cycle_ms(samples) if samples else 300.0
        need = 2 * cyc + 120     # (PERSIST_N-1) cycles + the intra-cycle stagger

        # With a hand-moved panel the dip span is set by how fast the operator
        # sweeps, so it is an INPUT, not a property of the system. Reported per
        # class it says what each sweep speed actually looked like in ms, which
        # is what makes the detection difference between classes interpretable.
        groups = []
        for t in pos:
            if t["cls"] not in groups:
                groups.append(t["cls"])
        for cl in groups:
            grp = [t for t in pos if t["cls"] == cl]
            spans = [t[f"dip{si + 1}_ms"] for t in grp for si in range(3)]
            w(describe(f"{cl} — beam dip span", spans, " ms") + "\n")
            cleared = sum(1 for t in grp
                          if min(t[f"dip{k + 1}_ms"] for k in range(3)) >= need)
            w(f"    {cleared}/{len(grp)} of these held all three beams the"
              f" ~{need:.0f} ms the midterm rule needs\n")

        floor = sum(1 for t in pos for si in range(3)
                    if t[f"dip{si + 1}_n"] == 1)
        total = len(pos) * 3
        w(f"\n  {floor}/{total} beam dips lasted a single sample"
          f" (<= {cyc:.0f} ms) — at or below the sampling\n"
          f"  resolution, so those are a measurement floor and not a real\n"
          f"  duration. A high count means the sweep is faster than the sensor\n"
          f"  round can resolve. Tier 4 made the round non-blocking but kept\n"
          f"  its {cyc:.0f} ms cadence on purpose, so PERSIST_N cycles means the\n"
          f"  same milliseconds in every capture. Shortening CYCLE_MS is now a\n"
          f"  one-constant change — but never mid-battery.\n")

    # ---- stability -------------------------------------------------------
    w("\n" + "-" * 78 + "\n")
    w("STABILITY\n")
    w("-" * 78 + "\n\n")
    closed = [s for s in samples if s["omask"] == 0 and s["hmask"] == 7]
    for si, key in enumerate(("d1", "d2", "d3")):
        w(describe(f"sensor {si + 1} closed-door",
                   [s[key] for s in closed], " cm") + "\n")
    w("\n")
    w(describe("free heap", [s["heap"] for s in samples], " B") + "\n")
    w(describe("RSSI", [s["rssi"] for s in samples if s["rssi"]], " dBm") + "\n")
    w(describe("temperature", [s["temp"] for s in samples], " C") + "\n")
    w(describe("humidity", [s["hum"] for s in samples], " %") + "\n")

    heaps = [s["heap"] for s in samples if s["heap"] == s["heap"]]
    if len(heaps) > 20:
        drift = heaps[-1] - heaps[0]
        w(f"\n  heap drift over capture    {drift:+.0f} B"
          f"  ({'no leak detected' if abs(drift) < 4096 else 'INVESTIGATE'})\n")

    faults = sum(1 for s in samples if s["hmask"] != 7)
    if samples:
        w(f"  samples with a sensor fault {faults}"
          f"  ({100.0 * faults / len(samples):.2f}%)\n")
    w("\n")


# ==========================================================================
# CSV export
# ==========================================================================

def export(sessions, trials, data_dir):
    os.makedirs(data_dir, exist_ok=True)
    written = []

    rows = [dict(s, session=i) for i, ses in enumerate(sessions)
            for s in ses["samples"]]
    if rows:
        p = os.path.join(data_dir, "samples.csv")
        with open(p, "w", newline="", encoding="utf-8") as fh:
            wr = csv.DictWriter(fh, fieldnames=["session"] + SAMPLE_FIELDS)
            wr.writeheader()
            wr.writerows(rows)
        written.append((p, len(rows)))

    evs = [dict(e, session=i) for i, ses in enumerate(sessions)
           for e in ses["events"]]
    if evs:
        p = os.path.join(data_dir, "events.csv")
        with open(p, "w", newline="", encoding="utf-8") as fh:
            wr = csv.DictWriter(fh, fieldnames=["session", "ms", "type", "detail"])
            wr.writeheader()
            wr.writerows(evs)
        written.append((p, len(evs)))

    if trials:
        p = os.path.join(data_dir, "trials.csv")
        with open(p, "w", newline="", encoding="utf-8") as fh:
            wr = csv.DictWriter(fh, fieldnames=list(trials[0].keys()))
            wr.writeheader()
            wr.writerows(trials)
        written.append((p, len(trials)))

    return written


# ==========================================================================
# Figures
# ==========================================================================

def figures(sessions, trials, out_dir):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        import matplotlib.patheffects as pe
    except ImportError:
        print("\n  matplotlib not installed — skipping figures.\n"
              "  pip install matplotlib\n")
        return []

    os.makedirs(out_dir, exist_ok=True)
    plt.rcParams.update({
        "figure.facecolor": SURFACE,
        "axes.facecolor": SURFACE,
        "savefig.facecolor": SURFACE,
        "font.family": "sans-serif",
        "font.size": 9,
        "text.color": INK,
        "axes.labelcolor": INK_2,
        "axes.edgecolor": BASELINE,
        "xtick.color": MUTED,
        "ytick.color": MUTED,
        "axes.grid": True,
        "grid.color": GRID,
        "grid.linewidth": 0.6,
        "axes.spines.top": False,
        "axes.spines.right": False,
        "legend.frameon": False,
    })

    made = []
    samples = [s for ses in sessions for s in ses["samples"]]

    def save(fig, name):
        p = os.path.join(out_dir, name)
        fig.tight_layout()
        fig.savefig(p, dpi=200)
        plt.close(fig)
        made.append(p)

    # ---- 1. distance trace of one real door opening ----------------------
    pos = [t for t in trials if t["expected"] == "alarm"]
    pick = next((t for t in pos if t["outcome"] == "TP"), pos[0] if pos else None)
    if pick:
        ses = next((s for s in sessions
                    if any(e["ms"] == pick["marker_ms"] for e in s["events"])),
                   sessions[0])
        # Clip at the next marker so a neighbouring trial's dip can't
        # wander into the frame and read as part of this event.
        later = [e["ms"] for e in ses["events"]
                 if e["type"] in ("trial_pos", "trial_neg")
                 and e["ms"] > pick["marker_ms"]]
        lo = pick["marker_ms"] - 1500
        hi = min(pick["marker_ms"] + 5000, min(later) - 300 if later else 1e18)
        win = [s for s in ses["samples"] if lo <= s["ms"] <= hi]
        if len(win) > 3:
            t0 = pick["marker_ms"]
            xs = [(s["ms"] - t0) / 1000.0 for s in win]
            fig, ax = plt.subplots(figsize=(7.2, 3.6))
            for si, key in enumerate(("d1", "d2", "d3")):
                ys = [s[key] for s in win]
                # A 2px surface-coloured halo under each line, so where the
                # three beams read almost the same distance they still
                # separate visually instead of the last-drawn one winning.
                ax.plot(xs, ys, color=SERIES[si], linewidth=2,
                        solid_capstyle="round", label=f"Sensor {si + 1}",
                        zorder=3 + si,
                        path_effects=[pe.Stroke(linewidth=4.5,
                                                foreground=SURFACE),
                                      pe.Normal()])
            thr = win[0]["thr"]
            ax.axhline(thr, color=MUTED, linewidth=1, linestyle=(0, (4, 3)))
            ax.annotate(f"trip threshold {thr:.0f} cm", (xs[0], thr),
                        xytext=(2, 4), textcoords="offset points",
                        color=MUTED, fontsize=8)
            if pick["alarm_ms"] != "":
                ax.axvline((pick["alarm_ms"] - t0) / 1000.0,
                           color=CRITICAL, linewidth=1.5, zorder=2)
                y0, y1 = ax.get_ylim()
                ax.annotate("alarm", ((pick["alarm_ms"] - t0) / 1000.0,
                                      y0 + 0.86 * (y1 - y0)),
                            xytext=(5, 0), textcoords="offset points",
                            color=CRITICAL, fontsize=8, fontweight="bold")
            ax.set_xlabel("seconds from trial marker")
            ax.set_ylabel("measured distance (cm)")
            ax.set_title("One door opening — the leaf sweeping through three beams",
                         color=INK, fontsize=10, loc="left", pad=10)
            # Legend below the plot: the three traces converge on the
            # closed-door reading, so anything inside the axes collides.
            ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.22),
                      ncol=3, handlelength=1.6)
            save(fig, "door_opening_trace.png")

    # ---- 2. detection latency ---------------------------------------------
    lat = [t["latency_detect_ms"] for t in trials
           if isinstance(t["latency_detect_ms"], (int, float))]
    if len(lat) >= 3:
        fig, ax = plt.subplots(figsize=(6.0, 3.2))
        ax.hist(lat, bins=min(12, max(4, len(lat) // 2)),
                color=SERIES[0], edgecolor=SURFACE, linewidth=1.5)
        m = stats.mean(lat)
        ax.axvline(m, color=CRITICAL, linewidth=1.5)
        sd = stats.stdev(lat) if len(lat) > 1 else 0.0
        ax.annotate(f"mean {m:.0f} ms   sd {sd:.0f} ms",
                    (m, ax.get_ylim()[1]), xytext=(6, -12),
                    textcoords="offset points", color=CRITICAL, fontsize=8,
                    fontweight="bold")
        ax.set_xlabel("first beam break to siren (ms)")
        ax.set_ylabel("trials")
        ax.set_title("Device-side detection latency", color=INK, fontsize=10,
                     loc="left", pad=10)
        ax.grid(axis="x", visible=False)
        save(fig, "detection_latency.png")

    # ---- 3. confusion matrix ---------------------------------------------
    # Four counts, not a magnitude surface: a labelled matrix, status-tinted,
    # with every cell carrying its own text label so colour is never alone.
    if trials:
        c = confusion(trials)
        fig, ax = plt.subplots(figsize=(5.0, 3.4))
        # x = what the trial marker expected, y = what the system did.
        # Keep these in step with the tick labels below or the matrix lies.
        cells = [
            (1, 1, "TP", c["TP"], "True positive",  GOOD),
            (1, 0, "FN", c["FN"], "Missed",         CRITICAL),
            (0, 1, "FP", c["FP"], "False alarm",    CRITICAL),
            (0, 0, "TN", c["TN"], "Correct reject", GOOD),
        ]
        for x, y, code, n, label, colour in cells:
            ax.add_patch(plt.Rectangle((x, y), 0.96, 0.96, facecolor=colour,
                                       alpha=0.14, edgecolor=colour,
                                       linewidth=1.2))
            ax.text(x + 0.48, y + 0.62, str(n), ha="center", va="center",
                    fontsize=24, fontweight="bold", color=INK)
            ax.text(x + 0.48, y + 0.26, f"{code} · {label}", ha="center",
                    va="center", fontsize=8, color=INK_2)
        ax.set_xlim(-0.05, 2.01)
        ax.set_ylim(-0.05, 2.01)
        ax.set_xticks([0.48, 1.48])
        ax.set_xticklabels(["no alarm expected", "alarm expected"])
        ax.set_yticks([0.48, 1.48])
        ax.set_yticklabels(["no alarm\nobserved", "alarm\nobserved"])
        ax.grid(False)
        for sp in ax.spines.values():
            sp.set_visible(False)
        ax.tick_params(length=0, colors=INK_2)
        r = rates(c)
        sub = "  ".join(f"{k} {r[k] * 100:.0f}%" for k in
                        ("recall", "precision", "specificity") if r[k] == r[k])
        ax.set_title(f"Detection outcomes\n{sub}", color=INK, fontsize=10,
                     loc="left", pad=10)
        save(fig, "confusion_matrix.png")

    # ---- 4. beam dip durations vs the persistence requirement -------------
    if pos:
        cyc = cycle_ms(samples) if samples else 300.0
        fig, ax = plt.subplots(figsize=(6.4, 3.4))
        for si in range(3):
            vals = [t[f"dip{si + 1}_ms"] for t in pos
                    if t[f"dip{si + 1}_ms"] > 0]
            if not vals:
                continue
            jitter = [si + (k % 7 - 3) * 0.035 for k in range(len(vals))]
            ax.scatter(jitter, vals, s=34, color=SERIES[si], alpha=0.75,
                       edgecolor=SURFACE, linewidth=1.2, zorder=3)
            med = stats.median(vals)
            ax.plot([si - 0.22, si + 0.22], [med, med], color=SERIES[si],
                    linewidth=2.5, solid_capstyle="round", zorder=4)
            ax.annotate(f"median {med:.0f} ms", (si, med),
                        xytext=(0, 7), textcoords="offset points",
                        color=SERIES[si], fontsize=8, fontweight="bold",
                        ha="center", va="bottom")
        need = 2 * cyc + 120
        ax.axhspan(0, need, color=CRITICAL, alpha=0.08, zorder=1)
        ax.axhline(need, color=CRITICAL, linewidth=1.2,
                   linestyle=(0, (4, 3)), zorder=2)
        ax.annotate(f"below here the midterm persistence rule cannot fire"
                    f"  (~{need:.0f} ms)", (-0.45, need), xytext=(0, 5),
                    textcoords="offset points", color=CRITICAL, fontsize=8,
                    fontweight="bold")
        ax.set_xticks(range(3))
        ax.set_xticklabels([f"Sensor {i + 1}" for i in range(3)])
        ax.set_ylabel("longest dip span (ms)")
        ax.set_xlim(-0.55, 2.55)
        ax.set_ylim(bottom=0)
        ax.set_title("How long each beam actually reads open during an opening",
                     color=INK, fontsize=10, loc="left", pad=10)
        ax.grid(axis="x", visible=False)
        save(fig, "dip_durations.png")

    # ---- 4b. risk score by trial class -----------------------------------
    rr = [t for t in trials if isinstance(t["risk_max"], (int, float))]
    if rr and any(t["risk_max"] for t in rr):
        fig, ax = plt.subplots(figsize=(6.0, 3.2))
        cats = []
        for t in rr:
            if t["cls"] not in cats:
                cats.append(t["cls"])
        for ci, cl in enumerate(cats):
            vals = [t["risk_max"] for t in rr if t["cls"] == cl]
            colour = SERIES[ci % len(SERIES)]
            jitter = [ci + (k % 7 - 3) * 0.035 for k in range(len(vals))]
            ax.scatter(jitter, vals, s=34, color=colour, alpha=0.75,
                       edgecolor=SURFACE, linewidth=1.2, zorder=3)
            med = stats.median(vals)
            ax.plot([ci - 0.22, ci + 0.22], [med, med], color=colour,
                    linewidth=2.5, solid_capstyle="round", zorder=4)
            ax.annotate(f"median {med:.0f}", (ci, med), xytext=(0, 7),
                        textcoords="offset points", color=colour, fontsize=8,
                        fontweight="bold", ha="center", va="bottom")
        for y, lab, col in ((30, "ELEVATED", WARNING), (70, "CRITICAL", CRITICAL)):
            ax.axhline(y, color=col, linewidth=1.2, linestyle=(0, (4, 3)), zorder=2)
            ax.annotate(lab, (-0.45, y), xytext=(0, 4), textcoords="offset points",
                        color=col, fontsize=8, fontweight="bold")
        ax.set_xticks(range(len(cats)))
        ax.set_xticklabels(cats)
        ax.set_ylabel("peak risk score")
        ax.set_xlim(-0.55, len(cats) - 0.45)
        ax.set_ylim(0, 105)
        ax.set_title("Risk score by trial class", color=INK, fontsize=10,
                     loc="left", pad=10)
        ax.grid(axis="x", visible=False)
        save(fig, "risk_by_class.png")

    # ---- 5. free heap over the capture -----------------------------------
    heaps = [(s["ms"] / 60000.0, s["heap"]) for s in samples
             if s["heap"] == s["heap"]]
    if len(heaps) > 30:
        fig, ax = plt.subplots(figsize=(6.4, 2.6))
        ax.plot([h[0] for h in heaps], [h[1] / 1024.0 for h in heaps],
                color=SERIES[0], linewidth=2, solid_capstyle="round")
        ax.set_xlabel("minutes since boot")
        ax.set_ylabel("free heap (KB)")
        ax.set_title("Free heap over the capture", color=INK, fontsize=10,
                     loc="left", pad=10)
        save(fig, "free_heap.png")

    return made


# ==========================================================================

def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", default=["logs/*.log"],
                    help="capture files or globs (default: logs/*.log)")
    ap.add_argument("--window", type=float, default=15.0,
                    help="seconds after a trial marker to attribute an alarm to it")
    ap.add_argument("--data", default="data", help="where to write tidy CSVs")
    ap.add_argument("--out", default="docs/figures", help="where to write figures")
    ap.add_argument("--no-figures", action="store_true")
    args = ap.parse_args()

    paths = sorted({p for pat in args.paths for p in glob.glob(pat)})
    if not paths:
        print(f"No capture files matched {args.paths}.\n"
              f"Record one with:  pio device monitor -f log2file")
        return 1

    print(f"Reading {len(paths)} file(s): " + ", ".join(os.path.basename(p)
                                                        for p in paths))
    sessions = parse(paths)
    if not sessions:
        print("No #D / #E records found. Is the firmware's CSV log enabled?\n"
              "Send 'c' in the serial monitor to toggle it.")
        return 1

    trials = [t for ses in sessions
              for t in build_trials(ses, args.window * 1000.0)]

    report(sessions, trials)

    for p, n in export(sessions, trials, args.data):
        print(f"  wrote {p}  ({n} rows)")
    if not args.no_figures:
        for p in figures(sessions, trials, args.out):
            print(f"  wrote {p}")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
