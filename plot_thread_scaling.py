#!/usr/bin/env python3
"""
Plot Clover+IS parallel speedup vs thread count.

Reads thread_scaling.csv (graph,threads,k,count,time_s) from
scripts/thread_scaling.sh and writes plots/thread_scaling.{png,svg}.

Usage:  python3 plot_thread_scaling.py thread_scaling.csv plots/
"""
import csv
import os
import sys
from collections import defaultdict

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

plt.rcParams.update({
    'font.family': 'serif',
    'font.size': 22,
    'axes.titlesize': 24,
    'axes.titleweight': 'bold',
    'axes.labelsize': 22,
    'xtick.labelsize': 20,
    'ytick.labelsize': 20,
    'legend.fontsize': 14,
    'lines.linewidth': 2.5,
    'lines.markersize': 10,
    'savefig.dpi': 300,
    'savefig.bbox': 'tight',
})

GRAPH_LABEL = {
    "com-LiveJournal": "LJ",
    "com-Orkut":       "Or",
    "com-Friendster":  "Fr",
    "hollywood-2009":  "Ho",
    "indochina-2004":  "In",
    "arabic-2005":     "Ar",
    "uk-2005":         "UK",
    "webbase-2001":    "Wb",
}
COLORS = ['#1f77b4', '#ff7f0e', '#2ca02c', '#d62728',
          '#9467bd', '#8c564b', '#e377c2', '#7f7f7f']


def load(path):
    data = defaultdict(dict)
    with open(path) as f:
        for row in csv.DictReader(f):
            try:
                t = int(row["threads"])
                tm = float(row["time_s"])
            except (ValueError, TypeError, KeyError):
                continue
            data[row["graph"]][t] = tm
    return {g: sorted(d.items()) for g, d in data.items()}


def plot_scaling(data, out_dir):
    graphs = [g for g in GRAPH_LABEL if g in data]
    if not graphs:
        print("No recognized graphs", file=sys.stderr)
        return
    fig, ax = plt.subplots(figsize=(8, 6))
    max_t = 1
    for i, g in enumerate(graphs):
        series = data[g]
        if not series:
            continue
        t1 = next((tm for t, tm in series if t == 1), None)
        if t1 is None:
            t1 = series[0][1] * series[0][0]
        ts, times = zip(*series)
        speedups = [t1 / tm for tm in times]
        max_t = max(max_t, ts[-1])
        ax.plot(ts, speedups, marker='o', label=GRAPH_LABEL[g], color=COLORS[i % len(COLORS)])
    lin = [1, max_t]
    ax.plot(lin, lin, '--', color='gray', label='ideal')
    ax.set_xscale('log', base=2)
    ax.set_yscale('log', base=2)
    ax.set_xlabel("Threads")
    ax.set_ylabel("Speedup (T₁ / Tₜ)")
    ax.grid(True, which='both', linestyle=':', alpha=0.4)
    ax.legend(loc='best', frameon=True, edgecolor='#cccccc', fancybox=False, ncol=2)
    fig.tight_layout()
    for ext in ('png', 'svg'):
        path = os.path.join(out_dir, f'thread_scaling.{ext}')
        fig.savefig(path, bbox_inches='tight')
        print(f"  wrote {path}")
    plt.close(fig)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    csv_path, out_dir = sys.argv[1], sys.argv[2]
    os.makedirs(out_dir, exist_ok=True)
    data = load(csv_path)
    if not data:
        print(f"No usable rows in {csv_path}", file=sys.stderr)
        sys.exit(1)
    plot_scaling(data, out_dir)


if __name__ == "__main__":
    main()
