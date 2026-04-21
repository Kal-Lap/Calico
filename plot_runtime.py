#!/usr/bin/env python3
"""
Plot per-(graph,k) counting time for PivotScale, Clover, and Clover+IS.

Usage:  python3 plot_runtime.py results.csv plots/
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
    'savefig.pad_inches': 0.05,
})

C_PS  = '#d32f2f'
C_COV = '#f9a825'
C_IS  = '#2e7d32'

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
BINARIES = ["pivotscale", "clover", "clover_is"]
BIN_LABEL = {"pivotscale": "PivotScale", "clover": "Clover", "clover_is": "Clover+IS"}
BIN_STYLE = {
    "pivotscale": dict(color=C_PS,  marker='^', linestyle='-'),
    "clover":     dict(color=C_COV, marker='s', linestyle='-'),
    "clover_is":  dict(color=C_IS,  marker='o', linestyle='-'),
}


def load(path):
    raw = defaultdict(lambda: defaultdict(dict))
    with open(path) as f:
        for row in csv.DictReader(f):
            try:
                t = float(row["time_s"])
                k = int(row["k"])
            except (ValueError, TypeError):
                continue
            raw[row["graph"]][row["binary"]][k] = t
    return {g: {b: sorted(bins[b].items()) for b in bins} for g, bins in raw.items()}


def plot_runtime(data, out_dir):
    graphs = [g for g in GRAPH_LABEL if g in data]
    if not graphs:
        print("No recognized graphs in data", file=sys.stderr)
        return
    fig, axes = plt.subplots(1, len(graphs), figsize=(4.0 * len(graphs), 4.5))
    if len(graphs) == 1:
        axes = [axes]
    for ax, g in zip(axes, graphs):
        for b in BINARIES:
            series = data[g].get(b, [])
            if not series:
                continue
            ks, ts = zip(*series)
            ax.plot(ks, ts, label=BIN_LABEL[b], **BIN_STYLE[b])
        ax.set_yscale("log")
        ax.set_title(GRAPH_LABEL[g])
        ax.set_xlabel("k")
        ax.grid(True, which="both", linestyle=":", alpha=0.4)
    axes[0].set_ylabel("Counting Time (s)")
    axes[-1].legend(loc="best", frameon=True, edgecolor='#cccccc', fancybox=False)
    fig.tight_layout()
    for ext in ('png', 'svg'):
        path = os.path.join(out_dir, f'runtime.{ext}')
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
    plot_runtime(data, out_dir)


if __name__ == "__main__":
    main()
