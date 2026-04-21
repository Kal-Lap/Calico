#!/usr/bin/env python3
"""
Plot Clover+IS runtime vs CD and SD thresholds.

Reads param_sweep.csv (graph,param,value,k,count,time_s) from
scripts/param_sweep.sh. Each point is normalized to the graph's CD=0 (SD=0)
baseline and plotted in two panels (CD on top, SD on bottom).

Usage:  python3 plot_param_sweep.py param_sweep.csv plots/
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
    'font.size': 18,
    'axes.labelsize': 18,
    'xtick.labelsize': 16,
    'ytick.labelsize': 16,
    'legend.fontsize': 12,
    'lines.linewidth': 2,
    'lines.markersize': 8,
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
    # data[param][graph] = sorted list of (value, time)
    data = defaultdict(lambda: defaultdict(dict))
    with open(path) as f:
        for row in csv.DictReader(f):
            try:
                v = int(row["value"])
                t = float(row["time_s"])
            except (ValueError, TypeError, KeyError):
                continue
            data[row["param"]][row["graph"]][v] = t
    return {p: {g: sorted(vs.items()) for g, vs in pd.items()} for p, pd in data.items()}


def plot_panel(ax, panel, xlabel):
    graphs = [g for g in GRAPH_LABEL if g in panel]
    for i, g in enumerate(graphs):
        series = panel[g]
        if not series:
            continue
        xs, ts = zip(*series)
        baseline = ts[0] if ts[0] > 0 else 1.0
        norm = [tm / baseline for tm in ts]
        ax.plot(xs, norm, marker='o', label=GRAPH_LABEL[g], color=COLORS[i % len(COLORS)])
    ax.axhline(1.0, linestyle=':', color='gray', alpha=0.6)
    ax.set_xlabel(xlabel)
    ax.set_ylabel("Runtime (normalized)")
    ax.grid(True, which='both', linestyle=':', alpha=0.4)


def plot_param_sweep(data, out_dir):
    if not data:
        return
    fig, axes = plt.subplots(2, 1, figsize=(8, 9))
    if "CD" in data:
        plot_panel(axes[0], data["CD"], "CD (complement degree threshold)")
    if "SD" in data:
        plot_panel(axes[1], data["SD"], "SD (subgraph density threshold, %)")
    axes[0].legend(loc='best', frameon=True, edgecolor='#cccccc', fancybox=False, ncol=2)
    fig.tight_layout()
    for ext in ('png', 'svg'):
        path = os.path.join(out_dir, f'param_sweep.{ext}')
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
    plot_param_sweep(data, out_dir)


if __name__ == "__main__":
    main()
