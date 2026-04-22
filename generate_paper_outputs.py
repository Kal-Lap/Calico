#!/usr/bin/env python3
"""Parse SLURM .out files and generate paper tables plus SVG figures.

Reviewer workflow:
  1. ./reproduce.sh
  2. python3 generate_paper_outputs.py

The parser is artifact-specific. The plot functions are copied from
../pivotcover/results/paper/generate_plots.py, with non-paper plots removed.
"""

import csv
import os
import re
from collections import defaultdict
from pathlib import Path

import pandas as pd
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
import numpy as np

# Style block from ../pivotcover/results/paper/generate_plots.py.
try:
    import seaborn as sns
    sns.set_theme(style="darkgrid", palette="dark")
    colors = sns.color_palette("dark")
except ImportError:
    colors = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd"]

ROOT = Path(__file__).resolve().parent
OUT_DIR = ROOT / "output"
RESULTS_DIR = ROOT / "results"
PAPER_DATA_DIR = RESULTS_DIR / "paper"
BENCHMARK_DIR = RESULTS_DIR / "benchmark"
STATS_DIR = RESULTS_DIR / "stats"
PLOTS_DIR = ROOT / "plots"
TABLES_DIR = ROOT / "tables"

for directory in (RESULTS_DIR, PAPER_DATA_DIR, BENCHMARK_DIR, STATS_DIR, PLOTS_DIR, TABLES_DIR):
    directory.mkdir(exist_ok=True)

plt.rcParams.update({"font.family": "serif"})
plt.rcParams.update({
    "font.size": 22,
    "font.weight": "normal",
    "axes.titlesize": 24,
    "axes.titleweight": "bold",
    "axes.labelsize": 22,
    "xtick.labelsize": 20,
    "ytick.labelsize": 20,
    "legend.fontsize": 14,
    "lines.linewidth": 2.5,
    "lines.markersize": 10,
    "savefig.dpi": 300,
    "savefig.bbox": "tight",
    "savefig.pad_inches": 0.05,
})

FIG_ROW5 = lambda h=4.5: (4.0 * 5, h)
FIG_SINGLE = (6, 4.5)

C_PS = "#d32f2f"
C_COV = "#f9a825"
C_IS = "#2e7d32"

CONFIG_STYLE = {
    "pivotscale_orig": {"label": "PivotScale", "color": C_PS, "marker": "^", "linestyle": "-"},
    "cover_only": {"label": "Clover", "color": C_COV, "marker": "s", "linestyle": "-"},
    "cover_is": {"label": "Clover+IS", "color": C_IS, "marker": "o", "linestyle": "-"},
}
CONFIGS = ["pivotscale_orig", "cover_only", "cover_is"]

ARTIFACT_CONFIGS = ["pivotscale", "clover", "clover_is"]
CONFIG_TO_PAPER = {
    "pivotscale": "pivotscale_orig",
    "clover": "cover_only",
    "clover_is": "cover_is",
}
CONFIG_TO_STATS_FILE = {
    "pivotscale": "pivotscale_orig_v2",
    "clover": "cover_only",
    "clover_is": "cover_is",
}
PAPER_TO_STATS_FILE = {
    "pivotscale_orig": "pivotscale_orig_v2",
    "cover_only": "cover_only",
    "cover_is": "cover_is",
}

GRAPH_SHORT = {
    "com-LiveJournal": "LJ",
    "com-Orkut": "Or",
    "indochina-2004": "In",
    "uk-2005": "UK",
    "webbase-2001": "Wb",
}
GRAPHS = list(GRAPH_SHORT.keys())

RUNTIME_GRAPHS = [
    "com-LiveJournal",
    "com-Orkut",
    "com-Friendster",
    "hollywood-2009",
    "indochina-2004",
    "arabic-2005",
    "uk-2005",
    "webbase-2001",
]
RUNTIME_GRAPH_SHORT = {
    **GRAPH_SHORT,
    "com-Friendster": "Fr",
    "hollywood-2009": "Ho",
    "arabic-2005": "Ar",
}
K_FIGURE = 7

OUTDIR = str(PLOTS_DIR)
STATSDIR = str(STATS_DIR)


def save(name):
    os.makedirs(OUTDIR, exist_ok=True)
    plt.savefig(os.path.join(OUTDIR, f"{name}.svg"), bbox_inches="tight")
    plt.close()
    print(f"  {name}")


def parse_kv(tokens):
    parsed = {}
    for token in tokens:
        if "=" in token:
            key, value = token.split("=", 1)
            parsed[key] = value
    return parsed


def parse_out(path):
    run = {
        "path": str(path),
        "meta": {},
        "time_s": None,
        "count": None,
        "summary": None,
        "time_bins": [],
        "density_bins": [],
    }
    with path.open(errors="replace") as handle:
        for line in handle:
            text = line.strip()
            if text.startswith("RUN "):
                run["meta"] = parse_kv(text.split()[1:])
            elif text.startswith("Counting Time:"):
                match = re.search(r"([\d.eE+-]+)", text.split(":", 1)[1])
                if match:
                    run["time_s"] = float(match.group(1))
            elif text.startswith("k:"):
                parts = text.split()
                try:
                    run["count"] = int(parts[-1])
                except (IndexError, ValueError):
                    pass
            elif text.startswith("STAT_SUMMARY"):
                run["summary"] = parse_kv(text.split()[1:])
            elif text.startswith("STAT_TIME_BIN"):
                run["time_bins"].append(parse_kv(text.split()[1:]))
            elif text.startswith("STAT_DENSITY_BIN"):
                run["density_bins"].append(parse_kv(text.split()[1:]))
    return run


def load_runs():
    buckets = defaultdict(list)
    if not OUT_DIR.exists():
        return buckets
    for path in sorted(OUT_DIR.rglob("*.out")):
        run = parse_out(path)
        kind = run["meta"].get("kind")
        if kind and run["time_s"] is not None:
            buckets[kind].append(run)
    return buckets


def mean_value(values):
    values = [value for value in values if value is not None]
    return sum(values) / len(values) if values else None


def mean_int(values):
    value = mean_value(values)
    return int(round(value)) if value is not None else 0


def paper_config(config):
    return CONFIG_TO_PAPER.get(config, config)


def stats_file_config(config):
    return CONFIG_TO_STATS_FILE.get(config, config)


def write_csv(path, rows, fieldnames):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def aggregate_runtime(runs):
    rows = []
    grouped = defaultdict(list)
    for run in runs:
        meta = run["meta"]
        graph = meta.get("graph")
        config = paper_config(meta.get("config"))
        k = int(meta.get("k", 0))
        rows.append({
            "graph": graph,
            "config": config,
            "k": k,
            "repeat": int(meta.get("repeat", 1)),
            "threads": int(meta.get("threads", 0)),
            "count": run["count"],
            "time_s": run["time_s"],
        })
        grouped[(graph, config, k)].append(run["time_s"])
    mean = {key: mean_value(values) for key, values in grouped.items()}
    return rows, mean


def aggregate_thread(runs):
    rows = []
    grouped = defaultdict(list)
    for run in runs:
        meta = run["meta"]
        graph = meta.get("graph")
        threads = int(meta.get("threads", 0))
        config = paper_config(meta.get("config", "clover_is"))
        rows.append({
            "graph": graph,
            "k": int(meta.get("k", K_FIGURE)),
            "config": config,
            "threads": threads,
            "repeat": int(meta.get("repeat", 1)),
            "counting_time_s": run["time_s"],
            "count": run["count"],
            "status": "ok",
        })
        if config == "cover_is":
            grouped[(graph, threads)].append(run["time_s"])
    mean = {key: mean_value(values) for key, values in grouped.items()}
    return rows, mean


def aggregate_param(runs):
    rows = []
    grouped = defaultdict(list)
    for run in runs:
        meta = run["meta"]
        graph = meta.get("graph")
        param = meta.get("param")
        try:
            value = int(meta.get("value"))
        except (TypeError, ValueError):
            continue
        rows.append({
            "graph": graph,
            "param": param,
            "value": value,
            "k": int(meta.get("k", K_FIGURE)),
            "repeat": int(meta.get("repeat", 1)),
            "time_s": run["time_s"],
            "count": run["count"],
        })
        grouped[(graph, param, value)].append(run["time_s"])
    mean = {key: mean_value(values) for key, values in grouped.items()}
    return rows, mean


def aggregate_stats(runs):
    rows = []
    time_rows = []
    density_rows = []
    grouped_stats = defaultdict(list)
    for run in runs:
        meta = run["meta"]
        graph = meta.get("graph")
        artifact_config = meta.get("config")
        config = paper_config(artifact_config)
        k = int(meta.get("k", 0))
        if run["summary"]:
            row = {
                "graph": graph,
                "config": config,
                "k": k,
                "tree_size": int(run["summary"].get("tree_size", 0)),
                "max_depth": int(run["summary"].get("max_depth", 0)),
                "runtime_192t_s": run["time_s"],
                "max_vertex_s": float(run["summary"].get("max_vertex_s", 0)),
                "total_work_s": float(run["summary"].get("total_work_s", 0)),
                "parallel_efficiency": float(run["summary"].get("parallel_efficiency", 0)),
            }
            rows.append(row)
            if k == K_FIGURE:
                grouped_stats[(graph, config)].append({
                    "time_s": run["time_s"],
                    "summary": row,
                    "time_bins": run["time_bins"],
                    "density_bins": run["density_bins"],
                })

        for item in run["time_bins"]:
            time_rows.append({
                "graph": graph,
                "config": config,
                "stats_config": stats_file_config(artifact_config),
                "k": k,
                "bin_low": float(item["bin_low"]),
                "bin_high": float(item["bin_high"]),
                "count": int(item["count"]),
                "time_sum_s": float(item.get("time_sum_s", 0.0)),
            })
        for item in run["density_bins"]:
            density_rows.append({
                "graph": graph,
                "config": config,
                "stats_config": stats_file_config(artifact_config),
                "k": k,
                "density_low": float(item["density_low"]),
                "density_high": float(item["density_high"]),
                "avg_time": float(item["avg_time"]),
                "vertex_count": int(item["vertex_count"]),
                "time_sum_s": float(item.get("time_sum_s", 0.0)),
            })
    mean_stats = {key: mean_stats_group(values) for key, values in grouped_stats.items()}
    return rows, time_rows, density_rows, mean_stats


def mean_stats_group(items):
    summaries = [item["summary"] for item in items]
    first = summaries[0]
    summary = {
        "graph": first["graph"],
        "config": first["config"],
        "k": first["k"],
        "tree_size": mean_int([row["tree_size"] for row in summaries]),
        "max_depth": mean_int([row["max_depth"] for row in summaries]),
        "runtime_192t_s": mean_value([row["runtime_192t_s"] for row in summaries]),
        "max_vertex_s": mean_value([row["max_vertex_s"] for row in summaries]),
        "total_work_s": mean_value([row["total_work_s"] for row in summaries]),
        "parallel_efficiency": mean_value([row["parallel_efficiency"] for row in summaries]),
    }
    return {
        "time_s": mean_value([item["time_s"] for item in items]),
        "summary": summary,
        "time_bins": mean_time_bins([item["time_bins"] for item in items]),
        "density_bins": mean_density_bins([item["density_bins"] for item in items]),
    }


def mean_time_bins(runs):
    grouped = defaultdict(list)
    for rows in runs:
        for row in rows:
            key = (float(row["bin_low"]), float(row["bin_high"]))
            grouped[key].append(row)
    merged = []
    for (low, high), rows in sorted(grouped.items()):
        merged.append({
            "bin_low": low,
            "bin_high": high,
            "count": mean_int([int(row["count"]) for row in rows]),
            "time_sum_s": mean_value([float(row.get("time_sum_s", 0.0)) for row in rows]),
        })
    return merged


def mean_density_bins(runs):
    grouped = defaultdict(list)
    for rows in runs:
        for row in rows:
            key = (float(row["density_low"]), float(row["density_high"]))
            grouped[key].append(row)
    merged = []
    for (low, high), rows in sorted(grouped.items()):
        merged.append({
            "density_low": low,
            "density_high": high,
            "avg_time": mean_value([float(row["avg_time"]) for row in rows]),
            "vertex_count": mean_int([int(row["vertex_count"]) for row in rows]),
            "time_sum_s": mean_value([float(row.get("time_sum_s", 0.0)) for row in rows]),
        })
    return merged


def fill_summary_runtime(stats_rows, mean_stats, mean_runtime):
    for row in stats_rows:
        if row["k"] == K_FIGURE:
            runtime = mean_runtime.get((row["graph"], row["config"], K_FIGURE))
            if runtime is not None:
                row["runtime_192t_s"] = runtime
    for (graph, config), item in mean_stats.items():
        runtime = mean_runtime.get((graph, config, K_FIGURE))
        if runtime is not None:
            item["summary"]["runtime_192t_s"] = runtime


def write_runtime_table(best):
    ks = list(range(3, 13))
    lines = [
        r"\begin{tabular}{ll" + "r" * len(ks) + "}",
        r"\toprule",
        "Graph & Algorithm & " + " & ".join(f"$k={k}$" for k in ks) + r" \\",
        r"\midrule",
    ]
    for graph in RUNTIME_GRAPHS:
        for index, config in enumerate(CONFIGS):
            label = CONFIG_STYLE[config]["label"]
            prefix = f"\\multirow{{3}}{{*}}{{{RUNTIME_GRAPH_SHORT.get(graph, graph)}}}" if index == 0 else ""
            cells = []
            for k in ks:
                time_s = best.get((graph, config, k))
                cells.append("--" if time_s is None else f"{time_s:.2f}")
            lines.append(f"{prefix} & {label} & " + " & ".join(cells) + r" \\")
        lines.append(r"\midrule" if graph != RUNTIME_GRAPHS[-1] else r"\bottomrule")
    lines.append(r"\end{tabular}")
    (TABLES_DIR / "runtime.tex").write_text("\n".join(lines) + "\n")


def write_speedup_table(best):
    ks = list(range(3, 13))
    lines = [
        r"\begin{tabular}{l" + "r" * len(ks) + "}",
        r"\toprule",
        "Graph & " + " & ".join(f"$k={k}$" for k in ks) + r" \\",
        r"\midrule",
    ]
    for graph in RUNTIME_GRAPHS:
        cells = []
        for k in ks:
            baseline = best.get((graph, "pivotscale_orig", k))
            candidates = [
                best.get((graph, "cover_only", k)),
                best.get((graph, "cover_is", k)),
            ]
            candidates = [value for value in candidates if value and value > 0]
            if baseline is None or not candidates:
                cells.append("--")
            else:
                cells.append(f"{baseline / min(candidates):.2f}$\\times$")
        lines.append(f"{RUNTIME_GRAPH_SHORT.get(graph, graph)} & " + " & ".join(cells) + r" \\")
    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    (TABLES_DIR / "speedup.tex").write_text("\n".join(lines) + "\n")


def mean_summary_rows(mean_stats):
    rows = []
    for graph in GRAPHS:
        for config in CONFIGS:
            item = mean_stats.get((graph, config))
            if item:
                rows.append(item["summary"])
    return rows


def write_paper_style_csvs(runtime_rows, thread_rows, param_rows, stats_rows,
                           time_rows, density_rows, mean_param, mean_stats):
    runtime_fields = ["graph", "config", "k", "repeat", "threads", "count", "time_s"]
    thread_fields = ["graph", "k", "config", "threads", "repeat", "counting_time_s", "count", "status"]
    param_fields = ["graph", "param", "value", "k", "repeat", "time_s", "count"]
    summary_fields = ["graph", "config", "k", "tree_size", "max_depth", "runtime_192t_s",
                      "max_vertex_s", "total_work_s", "parallel_efficiency"]

    write_csv(RESULTS_DIR / "runtime.csv", runtime_rows, runtime_fields)
    write_csv(RESULTS_DIR / "thread_scaling_k7.csv", thread_rows, thread_fields)
    write_csv(RESULTS_DIR / "param_sweep_k7.csv", param_rows, param_fields)
    summary_rows = mean_summary_rows(mean_stats)
    write_csv(RESULTS_DIR / "summary_k7.csv", summary_rows, summary_fields)
    write_csv(PAPER_DATA_DIR / "summary_k7.csv", summary_rows, summary_fields)
    write_csv(BENCHMARK_DIR / "thread_scaling_lj_k7.csv", thread_rows, thread_fields)

    write_csv(RESULTS_DIR / "stats_time_bins.csv",
              [{key: row[key] for key in ["graph", "config", "k", "bin_low", "bin_high", "count", "time_sum_s"]}
               for row in time_rows],
              ["graph", "config", "k", "bin_low", "bin_high", "count", "time_sum_s"])
    write_csv(RESULTS_DIR / "stats_density_bins.csv",
              [{key: row[key] for key in ["graph", "config", "k", "density_low", "density_high",
                                           "avg_time", "vertex_count", "time_sum_s"]}
               for row in density_rows],
              ["graph", "config", "k", "density_low", "density_high", "avg_time",
               "vertex_count", "time_sum_s"])

    sweep_d = []
    sweep_t = []
    for graph in GRAPHS:
        for (g, param, value), time_s in sorted(mean_param.items()):
            if g != graph or time_s is None:
                continue
            if param == "CD":
                sweep_d.append({"graph": graph, "D": value, "warm_s": time_s})
            elif param == "SD":
                sweep_t.append({"graph": graph, "T": value, "warm_s": time_s})
    write_csv(PAPER_DATA_DIR / "sweep_d_k7.csv", sweep_d, ["graph", "D", "warm_s"])
    write_csv(PAPER_DATA_DIR / "sweep_tau_combined_k7.csv", sweep_t, ["graph", "T", "warm_s"])

    for graph in GRAPHS:
        for config in CONFIGS:
            item = mean_stats.get((graph, config))
            stats_config = PAPER_TO_STATS_FILE[config]
            time_path = STATS_DIR / f"stats_{graph}_k7_{stats_config}_full_time_hist.csv"
            density_path = STATS_DIR / f"stats_{graph}_k7_{stats_config}_full_density_avg.csv"
            write_csv(time_path,
                      [{
                          "bin_low": float(row["bin_low"]),
                          "bin_high": float(row["bin_high"]),
                          "count": int(row["count"]),
                          "time_sum_s": float(row.get("time_sum_s", 0.0)),
                      } for row in (item["time_bins"] if item else [])],
                      ["bin_low", "bin_high", "count", "time_sum_s"])
            write_csv(density_path,
                      [{
                          "density_low": float(row["density_low"]),
                          "density_high": float(row["density_high"]),
                          "avg_time": float(row["avg_time"]),
                          "vertex_count": int(row["vertex_count"]),
                          "time_sum_s": float(row.get("time_sum_s", 0.0)),
                      } for row in (item["density_bins"] if item else [])],
                      ["density_low", "density_high", "avg_time", "vertex_count", "time_sum_s"])


def plot_runtime_tree_combined():
    df = pd.read_csv("summary_k7.csv")
    gnames = [GRAPH_SHORT[g] for g in GRAPHS]
    x = np.arange(len(GRAPHS))
    w = 0.25

    fig, (ax_rt, ax_tree, ax_dep) = plt.subplots(1, 3, figsize=(18, 4.5))

    for ax, metric, title in [
        (ax_rt, "runtime_192t_s", "Parallel Runtime (s)"),
        (ax_tree, "tree_size", "Search Tree Size"),
        (ax_dep, "max_depth", "Max Recursion Depth"),
    ]:
        for j, config in enumerate(CONFIGS):
            vals = []
            for g in GRAPHS:
                row = df[(df["graph"] == g) & (df["config"] == config)]
                vals.append(row[metric].values[0] if len(row) > 0 else 0)
            s = CONFIG_STYLE[config]
            ax.bar(x + (j - 1) * w, vals, w, label=s["label"], color=s["color"], alpha=0.85)
        ax.set_yscale("log")
        ax.set_title(title, fontsize=20)
        ax.set_xticks(x)
        ax.set_xticklabels(gnames, rotation=0, ha="center")

    ax_rt.legend(loc="upper left", fontsize=14, framealpha=0.8)
    fig.subplots_adjust(wspace=0.18)
    plt.tight_layout()
    save("plot_runtime_tree")


def plot_stacked_density_combined():
    import math
    ng = len(GRAPHS)
    import matplotlib.gridspec as gridspec
    fig = plt.figure(figsize=(3.8 * ng, 9))
    gs = gridspec.GridSpec(2, ng, hspace=0.50, wspace=0.22)

    ax_top0, ax_bot0 = None, None

    merge_map = {0: 0, 1: 0, 2: 0, 3: 1, 4: 1, 5: 1, 6: 2, 7: 2, 8: 3, 9: 3, 10: 4, 11: 4}
    merged_labels = ["< 1 ms", "1 ms - 1 s", "1 s - 100 s", "100 s - 10 ks", "> 10 ks"]
    n_merged = 5
    merged_colors = ["#2e7d32", "#8bc34a", "#fdd835", "#ef6c00", "#c62828"]
    hist_configs = [("pivotscale_orig_v2", "PivotScale"),
                    ("cover_only", "Clover"),
                    ("cover_is", "Clover+IS")]
    all_data = {}
    for graph in GRAPHS:
        for config, label in hist_configs:
            path = os.path.join(STATSDIR, f"stats_{graph}_k7_{config}_full_time_hist.csv")
            time_sums = np.zeros(n_merged)
            if os.path.exists(path):
                with open(path) as fh:
                    for idx, row in enumerate(csv.DictReader(fh)):
                        lo, hi, count = float(row["bin_low"]), float(row["bin_high"]), int(row["count"])
                        if count == 0: continue
                        if lo == 0: t = count * hi / 2
                        else: t = count * math.sqrt(lo * hi)
                        if idx in merge_map:
                            time_sums[merge_map[idx]] += t
            all_data[(graph, label)] = time_sums
    nc = len(hist_configs)
    for i, graph in enumerate(GRAPHS):
        ax = fig.add_subplot(gs[0, i], sharey=ax_top0)
        if i == 0: ax_top0 = ax
        for j, (config, label) in enumerate(hist_configs):
            tsums = all_data[(graph, label)]
            bottom = 0
            for b in range(n_merged):
                if tsums[b] > 0:
                    ax.bar(j, tsums[b], bottom=bottom, width=0.65,
                           color=merged_colors[b], edgecolor="white", linewidth=0.5)
                    bottom += tsums[b]
        ax.set_yscale("log")
        ax.set_xticks(range(nc))
        ax.set_xticklabels(["PivotScale", "Clover", "Clover+IS"],
                           fontsize=plt.rcParams["xtick.labelsize"] * 0.55,
                           rotation=0, ha="center")
        ax.set_title(GRAPH_SHORT[graph])
        if i == 0:
            ax.set_ylabel("Counting Time (s)")
            ax.yaxis.set_label_coords(-0.30, 0.5)
        else:
            ax.tick_params(labelleft=False)

    from matplotlib.patches import Patch
    legend_patches = [Patch(facecolor=merged_colors[b], label=merged_labels[b]) for b in range(n_merged)]
    fig.legend(handles=legend_patches, loc="center", ncol=n_merged,
               bbox_to_anchor=(0.5, 0.50),
               frameon=True, edgecolor="#cccccc", fancybox=False,
               handlelength=2.0, columnspacing=1.2,
               title="Per-vertex counting time:")

    density_configs = [("pivotscale_orig_v2", C_PS, "PivotScale", "^"),
                       ("cover_only", C_COV, "Clover", "s"),
                       ("cover_is", C_IS, "Clover+IS", "o")]
    for i, graph in enumerate(GRAPHS):
        ax = fig.add_subplot(gs[1, i], sharey=ax_bot0)
        if i == 0: ax_bot0 = ax
        for config, color, label, marker in density_configs:
            path = os.path.join(STATSDIR, f"stats_{graph}_k7_{config}_full_density_avg.csv")
            if not os.path.exists(path): continue
            lows, highs, avgs, counts = [], [], [], []
            with open(path) as fh:
                for row in csv.DictReader(fh):
                    lows.append(float(row["density_low"]))
                    highs.append(float(row["density_high"]))
                    avgs.append(float(row["avg_time"]))
                    counts.append(int(row["vertex_count"]))
            centers, merged_vals = [], []
            for b in range(0, len(lows), 2):
                if b + 1 < len(lows):
                    c1, c2 = counts[b], counts[b + 1]
                    total = c1 + c2
                    if total > 0 and (avgs[b] > 0 or avgs[b + 1] > 0):
                        centers.append((lows[b] + highs[b + 1]) / 2)
                        merged_vals.append((avgs[b] * c1 + avgs[b + 1] * c2) / total)
            valid = [(c, a) for c, a in zip(centers, merged_vals) if a > 0]
            if valid:
                xs, ys = zip(*valid)
                ax.fill_between(xs, ys, alpha=0.10, color=color)
                ax.plot(xs, ys, marker=marker, color=color, label=label, markersize=8,
                        linewidth=2.0, alpha=0.85, markeredgecolor="white", markeredgewidth=0.4)
        ax.set_yscale("log")
        ax.set_xlim(-0.03, 1.03)
        ax.set_xticks([0.0, 0.2, 0.4, 0.6, 0.8, 1.0])
        ax.tick_params(axis="x", labelsize=16)
        if i == 0:
            ax.set_ylabel("Avg counting time (s)")
            ax.yaxis.set_label_coords(-0.30, 0.5)
            ax.legend(loc="upper left", framealpha=0.8)
        else:
            ax.tick_params(labelleft=False)
        if i == ng // 2:
            ax.set_xlabel("Subgraph density")
    save("plot_stacked_density")


def plot_scaling_time_is_only():
    df = pd.read_csv("../benchmark/thread_scaling_lj_k7.csv")
    df = df[(df["status"] == "ok") & (df["config"] == "cover_is")]
    show_threads = np.array([1, 4, 16, 64, 192])
    graphs = [g for g in GRAPHS if g in df["graph"].unique()]
    ng = len(graphs)
    fig, axes = plt.subplots(1, ng, figsize=(4.0 * ng, 5.0), sharey=True, squeeze=False)
    axes = axes[0]
    for idx, graph in enumerate(graphs):
        ax = axes[idx]
        cdf = df[df["graph"] == graph].sort_values("threads")
        cdf = cdf[cdf["threads"].isin(show_threads)]
        t1 = cdf[cdf["threads"] == 1]["counting_time_s"].values[0]
        speedup = t1 / cdf["counting_time_s"].values
        ax.plot(cdf["threads"].values, speedup,
                marker="o", color=C_IS, linewidth=2.5, markersize=9,
                markeredgecolor="white", markeredgewidth=0.5)
        ax.plot(show_threads, show_threads, "k--", linewidth=1.0, alpha=0.4, label="Ideal" if idx == 0 else None)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log", base=2)
        ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
        ax.yaxis.set_major_formatter(ticker.FuncFormatter(lambda x, _: f"{int(x)}" if x >= 1 else f"{x:.1f}"))
        ax.set_xticks(show_threads)
        ax.set_xlim(0.7, 280)
        ax.set_title(GRAPH_SHORT[graph])
        ax.grid(True, which="major", alpha=0.2)
        if idx == 0:
            ax.set_ylabel("Parallel Speedup")
            ax.legend(loc="upper left", framealpha=0.8)
    fig.text(0.5, -0.02, "Number of Threads", ha="center")
    plt.tight_layout()
    save("thread_scaling_is_only")


def plot_param_sweep():
    graph_colors = {
        "com-LiveJournal": "#001C7F",
        "com-Orkut": "#B1400D",
        "indochina-2004": "#12711C",
        "uk-2005": "#8C0800",
        "webbase-2001": "#591E71",
    }
    fig, (ax_d, ax_t) = plt.subplots(2, 1, figsize=(7, 8), gridspec_kw={"hspace": 0.18})

    df_d = pd.read_csv("sweep_d_k7.csv")
    for graph in GRAPHS:
        gdf = df_d[df_d["graph"] == graph].sort_values("D")
        baseline = gdf[gdf["D"] == 0]["warm_s"].values[0]
        ax_d.plot(gdf["D"], gdf["warm_s"] / baseline, marker="o", color=graph_colors[graph],
                  label=GRAPH_SHORT[graph], linewidth=2.0, markersize=7,
                  markeredgecolor="white", markeredgewidth=0.5)
    ax_d.xaxis.set_label_position("top")
    ax_d.xaxis.tick_top()
    ax_d.set_xlabel(r"Complement Degree threshold (CD)", fontsize=20, labelpad=10)
    ax_d.set_ylabel(r"$T(\mathrm{CD}) \,/\, T(\mathrm{CD}{=}0)$", fontsize=20)
    ax_d.set_xticks([0, 10, 20, 30, 40, 50])
    ax_d.axhline(y=1.0, color="black", linestyle="-", linewidth=0.5, alpha=0.3)
    ax_d.axvline(x=20, color="gray", linestyle="--", linewidth=0.7, alpha=0.5)
    ax_d.set_ylim(0, 2.5)
    ax_d.set_xlim(-1, 52)
    ax_d.grid(True, which="major", alpha=0.2)

    df_t = pd.read_csv("sweep_tau_combined_k7.csv")
    for graph in df_t["graph"].unique():
        gdf = df_t[df_t["graph"] == graph].sort_values("T")
        baseline = gdf[gdf["T"] == 0]["warm_s"].values[0]
        ax_t.plot(gdf["T"], gdf["warm_s"] / baseline, marker="s", color=graph_colors[graph],
                  label=GRAPH_SHORT[graph], linewidth=2.0, markersize=7,
                  markeredgecolor="white", markeredgewidth=0.5)
    ax_t.set_xlabel(r"Subgraph Density threshold (SD)", fontsize=20)
    ax_t.axhline(y=1.0, color="black", linestyle="-", linewidth=0.5, alpha=0.3)
    ax_t.axvline(x=75, color="gray", linestyle="--", linewidth=0.7, alpha=0.5)
    ax_t.set_ylim(0, 1.2)
    ax_t.set_xlim(0, 95)
    ax_t.set_xticks([0, 20, 40, 60, 80])
    ax_t.set_ylabel(r"$T(\mathrm{SD}) \,/\, T(\mathrm{SD}{=}0)$", fontsize=20)
    ax_t.grid(True, which="major", alpha=0.2)

    ax_d.legend(loc="upper left", fontsize=14, framealpha=0.8, ncol=2)
    plt.tight_layout()
    save("plot_param_sweep")


def expected_runtime_keys():
    return {(graph, config, k)
            for graph in RUNTIME_GRAPHS
            for config in CONFIGS
            for k in range(3, 13)}


def report_missing(mean_runtime, mean_stats, mean_thread, mean_param):
    missing = []
    runtime_missing = expected_runtime_keys() - set(mean_runtime)
    if runtime_missing:
        missing.append(f"runtime rows missing: {len(runtime_missing)}")
    stats_missing = {(graph, config) for graph in GRAPHS for config in CONFIGS} - set(mean_stats)
    if stats_missing:
        missing.append(f"k=7 stats runs missing: {len(stats_missing)}")
    thread_missing = {(graph, thread) for graph in GRAPHS for thread in [1, 4, 16, 64, 192]} - set(mean_thread)
    if thread_missing:
        missing.append(f"thread-scaling points missing: {len(thread_missing)}")
    cd_missing = {(graph, value) for graph in GRAPHS for value in [0, 5, 10, 15, 20, 25, 30, 40, 50]
                  if (graph, "CD", value) not in mean_param}
    sd_missing = {(graph, value) for graph in GRAPHS for value in [0, 30, 40, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95]
                  if (graph, "SD", value) not in mean_param}
    if cd_missing or sd_missing:
        missing.append(f"parameter-sweep points missing: {len(cd_missing) + len(sd_missing)}")

    if missing:
        print("Incomplete data detected:")
        for item in missing:
            print(f"  - {item}")


def available_thread_graphs():
    path = PAPER_DATA_DIR / "../benchmark/thread_scaling_lj_k7.csv"
    df = pd.read_csv(path)
    df = df[(df["status"] == "ok") & (df["config"] == "cover_is")]
    return [g for g in GRAPHS if len(df[(df["graph"] == g) & (df["threads"] == 1)]) > 0]


def available_stats_graphs():
    df = pd.read_csv(PAPER_DATA_DIR / "summary_k7.csv")
    return [g for g in GRAPHS if g in df["graph"].unique()]


def available_param_graphs():
    df_d = pd.read_csv(PAPER_DATA_DIR / "sweep_d_k7.csv")
    df_t = pd.read_csv(PAPER_DATA_DIR / "sweep_tau_combined_k7.csv")
    return [g for g in GRAPHS
            if len(df_d[(df_d["graph"] == g) & (df_d["D"] == 0)]) > 0
            and len(df_t[(df_t["graph"] == g) & (df_t["T"] == 0)]) > 0]


def with_graphs(graphs, plot_func, plot_name):
    global GRAPHS
    if not graphs:
        print(f"  skip {plot_name}: missing required baseline data")
        return
    old_graphs = GRAPHS
    GRAPHS = graphs
    try:
        plot_func()
    finally:
        GRAPHS = old_graphs


def generate_paper_plots():
    old_cwd = Path.cwd()
    os.chdir(PAPER_DATA_DIR)
    try:
        print("Generating paper plots...")
        if available_stats_graphs():
            plot_runtime_tree_combined()
            plot_stacked_density_combined()
        else:
            print("  skip plot_runtime_tree: missing required baseline data")
            print("  skip plot_stacked_density: missing required baseline data")
        if available_thread_graphs():
            plot_scaling_time_is_only()
        else:
            print("  skip thread_scaling_is_only: missing required baseline data")
        with_graphs(available_param_graphs(), plot_param_sweep, "plot_param_sweep")
    finally:
        os.chdir(old_cwd)


def main():
    buckets = load_runs()
    if not buckets:
        print(f"no .out files under {OUT_DIR}")
        return

    runtime_rows, mean_runtime = aggregate_runtime(buckets.get("runtime", []))
    thread_rows, mean_thread = aggregate_thread(buckets.get("thread", []))
    param_rows, mean_param = aggregate_param(buckets.get("param", []))
    stats_rows, time_rows, density_rows, mean_stats = aggregate_stats(buckets.get("stats", []))

    fill_summary_runtime(stats_rows, mean_stats, mean_runtime)

    if runtime_rows:
        write_runtime_table(mean_runtime)
        write_speedup_table(mean_runtime)
    write_paper_style_csvs(runtime_rows, thread_rows, param_rows, stats_rows,
                           time_rows, density_rows, mean_param, mean_stats)

    generate_paper_plots()
    report_missing(mean_runtime, mean_stats, mean_thread, mean_param)
    print(f"results -> {RESULTS_DIR.relative_to(ROOT)}")
    print(f"tables  -> {TABLES_DIR.relative_to(ROOT)}")
    print(f"plots   -> {PLOTS_DIR.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
