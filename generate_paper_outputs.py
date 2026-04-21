#!/usr/bin/env python3
"""Parse SLURM .out files and generate the paper tables + SVG figures.

Reviewer workflow:
  1. ./reproduce.sh
  2. python3 generate_paper_outputs.py

The plotting code intentionally mirrors the paper generator in
../pivotcover/results/paper/generate_plots.py, but reads the reproducibility
artifact's output/**/*.out files instead of private development CSVs.
"""

import csv
import re
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.gridspec as gridspec
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker
from matplotlib.patches import Patch
import numpy as np

ROOT = Path(__file__).resolve().parent
OUT_DIR = ROOT / "output"
RESULTS_DIR = ROOT / "results"
PLOTS_DIR = ROOT / "plots"
TABLES_DIR = ROOT / "tables"

for directory in (RESULTS_DIR, PLOTS_DIR, TABLES_DIR):
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

C_PS = "#d32f2f"
C_COV = "#f9a825"
C_IS = "#2e7d32"

CONFIG_STYLE = {
    "pivotscale": {"label": "PivotScale", "color": C_PS, "marker": "^", "linestyle": "-"},
    "clover": {"label": "Clover", "color": C_COV, "marker": "s", "linestyle": "-"},
    "clover_is": {"label": "Clover+IS", "color": C_IS, "marker": "o", "linestyle": "-"},
}
CONFIGS = ["pivotscale", "clover", "clover_is"]

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
FIGURE_GRAPHS = [
    "com-LiveJournal",
    "com-Orkut",
    "indochina-2004",
    "uk-2005",
    "webbase-2001",
]
GRAPH_SHORT = {
    "com-LiveJournal": "LJ",
    "com-Orkut": "Or",
    "com-Friendster": "Fr",
    "hollywood-2009": "Ho",
    "indochina-2004": "In",
    "arabic-2005": "Ar",
    "uk-2005": "UK",
    "webbase-2001": "Wb",
}
K_FIGURE = 7


def save_svg(name):
    path = PLOTS_DIR / f"{name}.svg"
    plt.savefig(path, bbox_inches="tight")
    plt.close()
    print(f"  wrote {path.relative_to(ROOT)}")


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


def best_time(values):
    values = [value for value in values if value is not None]
    return min(values) if values else None


def write_csv(path, rows, fieldnames):
    with path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def aggregate_runtime(runs):
    rows = []
    grouped = defaultdict(list)
    counts = {}
    for run in runs:
        meta = run["meta"]
        graph = meta.get("graph")
        config = meta.get("config")
        k = int(meta.get("k", 0))
        repeat = int(meta.get("repeat", 1))
        threads = int(meta.get("threads", 0))
        rows.append({
            "graph": graph,
            "config": config,
            "k": k,
            "repeat": repeat,
            "threads": threads,
            "count": run["count"],
            "time_s": run["time_s"],
        })
        grouped[(graph, config, k)].append(run["time_s"])
        counts[(graph, config, k)] = run["count"]

    best = {key: best_time(values) for key, values in grouped.items()}
    return rows, best, counts


def aggregate_thread(runs):
    rows = []
    grouped = defaultdict(list)
    for run in runs:
        meta = run["meta"]
        graph = meta.get("graph")
        threads = int(meta.get("threads", 0))
        rows.append({
            "graph": graph,
            "k": int(meta.get("k", K_FIGURE)),
            "config": meta.get("config", "clover_is"),
            "threads": threads,
            "repeat": int(meta.get("repeat", 1)),
            "counting_time_s": run["time_s"],
            "count": run["count"],
            "status": "ok",
        })
        grouped[(graph, threads)].append(run["time_s"])
    best = {key: best_time(values) for key, values in grouped.items()}
    return rows, best


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
    best = {key: best_time(values) for key, values in grouped.items()}
    return rows, best


def aggregate_stats(runs):
    rows = []
    time_rows = []
    density_rows = []
    best = {}
    for run in runs:
        meta = run["meta"]
        graph = meta.get("graph")
        config = meta.get("config")
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
            key = (graph, config)
            if k == K_FIGURE and (key not in best or run["time_s"] < best[key]["time_s"]):
                best[key] = {
                    "time_s": run["time_s"],
                    "summary": row,
                    "time_bins": run["time_bins"],
                    "density_bins": run["density_bins"],
                }

        for item in run["time_bins"]:
            time_rows.append({
                "graph": graph,
                "config": config,
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
                "k": k,
                "density_low": float(item["density_low"]),
                "density_high": float(item["density_high"]),
                "avg_time": float(item["avg_time"]),
                "vertex_count": int(item["vertex_count"]),
                "time_sum_s": float(item.get("time_sum_s", 0.0)),
            })
    return rows, time_rows, density_rows, best


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
            prefix = f"\\multirow{{3}}{{*}}{{{GRAPH_SHORT.get(graph, graph)}}}" if index == 0 else ""
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
            baseline = best.get((graph, "pivotscale", k))
            candidates = [
                best.get((graph, "clover", k)),
                best.get((graph, "clover_is", k)),
            ]
            candidates = [value for value in candidates if value and value > 0]
            if baseline is None or not candidates:
                cells.append("--")
            else:
                cells.append(f"{baseline / min(candidates):.2f}$\\times$")
        lines.append(f"{GRAPH_SHORT.get(graph, graph)} & " + " & ".join(cells) + r" \\")
    lines.append(r"\bottomrule")
    lines.append(r"\end{tabular}")
    (TABLES_DIR / "speedup.tex").write_text("\n".join(lines) + "\n")


def write_paper_style_csvs(runtime_rows, thread_rows, param_rows, stats_rows, time_rows, density_rows):
    write_csv(RESULTS_DIR / "runtime.csv", runtime_rows,
              ["graph", "config", "k", "repeat", "threads", "count", "time_s"])
    write_csv(RESULTS_DIR / "thread_scaling_k7.csv", thread_rows,
              ["graph", "k", "config", "threads", "repeat", "counting_time_s", "count", "status"])
    write_csv(RESULTS_DIR / "param_sweep_k7.csv", param_rows,
              ["graph", "param", "value", "k", "repeat", "time_s", "count"])
    write_csv(RESULTS_DIR / "summary_k7.csv", stats_rows,
              ["graph", "config", "k", "tree_size", "max_depth", "runtime_192t_s",
               "max_vertex_s", "total_work_s", "parallel_efficiency"])
    write_csv(RESULTS_DIR / "stats_time_bins.csv", time_rows,
              ["graph", "config", "k", "bin_low", "bin_high", "count", "time_sum_s"])
    write_csv(RESULTS_DIR / "stats_density_bins.csv", density_rows,
              ["graph", "config", "k", "density_low", "density_high", "avg_time",
               "vertex_count", "time_sum_s"])

    for graph in FIGURE_GRAPHS:
        for config in CONFIGS:
            time_path = RESULTS_DIR / f"stats_{graph}_k7_{config}_full_time_hist.csv"
            density_path = RESULTS_DIR / f"stats_{graph}_k7_{config}_full_density_avg.csv"
            write_csv(time_path,
                      [row for row in time_rows if row["graph"] == graph and row["config"] == config and row["k"] == K_FIGURE],
                      ["graph", "config", "k", "bin_low", "bin_high", "count", "time_sum_s"])
            write_csv(density_path,
                      [row for row in density_rows if row["graph"] == graph and row["config"] == config and row["k"] == K_FIGURE],
                      ["graph", "config", "k", "density_low", "density_high", "avg_time",
                       "vertex_count", "time_sum_s"])


def plot_runtime_tree_combined(best_stats, best_runtime):
    graphs = [graph for graph in FIGURE_GRAPHS if any((graph, config) in best_stats for config in CONFIGS)]
    if not graphs:
        print("  skip plot_runtime_tree.svg: no stats data")
        return

    x = np.arange(len(graphs))
    width = 0.25
    fig, (ax_rt, ax_tree, ax_dep) = plt.subplots(1, 3, figsize=(18, 4.5))

    for ax, metric, title in [
        (ax_rt, "runtime_192t_s", "Parallel Runtime (s)"),
        (ax_tree, "tree_size", "Search Tree Size"),
        (ax_dep, "max_depth", "Max Recursion Depth"),
    ]:
        for offset, config in enumerate(CONFIGS):
            values = []
            for graph in graphs:
                item = best_stats.get((graph, config))
                if metric == "runtime_192t_s":
                    values.append(best_runtime.get((graph, config, K_FIGURE), np.nan))
                else:
                    values.append(item["summary"][metric] if item else np.nan)
            style = CONFIG_STYLE[config]
            ax.bar(x + (offset - 1) * width, values, width,
                   label=style["label"], color=style["color"], alpha=0.85)
        ax.set_yscale("log")
        ax.set_title(title, fontsize=20)
        ax.set_xticks(x)
        ax.set_xticklabels([GRAPH_SHORT[graph] for graph in graphs], rotation=0, ha="center")

    ax_rt.legend(loc="upper left", fontsize=14, framealpha=0.8)
    fig.subplots_adjust(wspace=0.18)
    plt.tight_layout()
    save_svg("plot_runtime_tree")


def plot_stacked_density_combined(best_stats):
    graphs = [graph for graph in FIGURE_GRAPHS if any((graph, config) in best_stats for config in CONFIGS)]
    if not graphs:
        print("  skip plot_stacked_density.svg: no stats data")
        return

    fig = plt.figure(figsize=(3.8 * len(graphs), 9))
    gs = gridspec.GridSpec(2, len(graphs), hspace=0.50, wspace=0.22)

    merge_map = {0: 0, 1: 0, 2: 0, 3: 1, 4: 1, 5: 1, 6: 2, 7: 2, 8: 3, 9: 3, 10: 4, 11: 4}
    merged_labels = ["< 1 ms", "1 ms - 1 s", "1 s - 100 s", "100 s - 10 ks", "> 10 ks"]
    merged_colors = ["#2e7d32", "#8bc34a", "#fdd835", "#ef6c00", "#c62828"]
    n_merged = len(merged_labels)

    ax_top0 = None
    for graph_index, graph in enumerate(graphs):
        ax = fig.add_subplot(gs[0, graph_index], sharey=ax_top0)
        if graph_index == 0:
            ax_top0 = ax
        for config_index, config in enumerate(CONFIGS):
            item = best_stats.get((graph, config))
            time_sums = np.zeros(n_merged)
            if item:
                for bin_index, row in enumerate(item["time_bins"]):
                    if bin_index in merge_map:
                        time_sums[merge_map[bin_index]] += float(row.get("time_sum_s", 0.0))
            bottom = 0.0
            for bin_index in range(n_merged):
                if time_sums[bin_index] > 0:
                    ax.bar(config_index, time_sums[bin_index], bottom=bottom, width=0.65,
                           color=merged_colors[bin_index], edgecolor="white", linewidth=0.5)
                    bottom += time_sums[bin_index]
        ax.set_yscale("log")
        ax.set_xticks(range(len(CONFIGS)))
        ax.set_xticklabels([CONFIG_STYLE[config]["label"] for config in CONFIGS],
                           fontsize=plt.rcParams["xtick.labelsize"] * 0.55,
                           rotation=0, ha="center")
        ax.set_title(GRAPH_SHORT[graph])
        if graph_index == 0:
            ax.set_ylabel("Counting Time (s)")
            ax.yaxis.set_label_coords(-0.30, 0.5)
        else:
            ax.tick_params(labelleft=False)

    legend_patches = [Patch(facecolor=merged_colors[index], label=merged_labels[index])
                      for index in range(n_merged)]
    fig.legend(handles=legend_patches, loc="center", ncol=n_merged,
               bbox_to_anchor=(0.5, 0.50), frameon=True, edgecolor="#cccccc",
               fancybox=False, handlelength=2.0, columnspacing=1.2,
               title="Per-vertex counting time:")

    ax_bot0 = None
    for graph_index, graph in enumerate(graphs):
        ax = fig.add_subplot(gs[1, graph_index], sharey=ax_bot0)
        if graph_index == 0:
            ax_bot0 = ax
        for config in CONFIGS:
            item = best_stats.get((graph, config))
            if not item:
                continue
            lows = [float(row["density_low"]) for row in item["density_bins"]]
            highs = [float(row["density_high"]) for row in item["density_bins"]]
            avgs = [float(row["avg_time"]) for row in item["density_bins"]]
            counts = [int(row["vertex_count"]) for row in item["density_bins"]]
            centers = []
            merged_values = []
            for bin_index in range(0, len(lows), 2):
                if bin_index + 1 >= len(lows):
                    continue
                c1 = counts[bin_index]
                c2 = counts[bin_index + 1]
                total = c1 + c2
                if total > 0 and (avgs[bin_index] > 0 or avgs[bin_index + 1] > 0):
                    centers.append((lows[bin_index] + highs[bin_index + 1]) / 2)
                    merged_values.append((avgs[bin_index] * c1 + avgs[bin_index + 1] * c2) / total)
            valid = [(center, value) for center, value in zip(centers, merged_values) if value > 0]
            if valid:
                xs, ys = zip(*valid)
                style = CONFIG_STYLE[config]
                ax.fill_between(xs, ys, alpha=0.10, color=style["color"])
                ax.plot(xs, ys, marker=style["marker"], color=style["color"],
                        label=style["label"], markersize=8, linewidth=2.0,
                        alpha=0.85, markeredgecolor="white", markeredgewidth=0.4)
        ax.set_yscale("log")
        ax.set_xlim(-0.03, 1.03)
        ax.set_xticks([0.0, 0.2, 0.4, 0.6, 0.8, 1.0])
        ax.tick_params(axis="x", labelsize=16)
        if graph_index == 0:
            ax.set_ylabel("Avg counting time (s)")
            ax.yaxis.set_label_coords(-0.30, 0.5)
            ax.legend(loc="upper left", framealpha=0.8)
        else:
            ax.tick_params(labelleft=False)
        if graph_index == len(graphs) // 2:
            ax.set_xlabel("Subgraph density")

    save_svg("plot_stacked_density")


def plot_scaling_time_is_only(best_thread):
    graphs = [graph for graph in FIGURE_GRAPHS if any(key[0] == graph for key in best_thread)]
    if not graphs:
        print("  skip thread_scaling_is_only.svg: no thread data")
        return

    show_threads = np.array([1, 4, 16, 64, 192])
    fig, axes = plt.subplots(1, len(graphs), figsize=(4.0 * len(graphs), 5.0),
                             sharey=True, squeeze=False)
    axes = axes[0]

    for index, graph in enumerate(graphs):
        ax = axes[index]
        pairs = [(thread, best_thread[(graph, thread)]) for thread in show_threads
                 if (graph, thread) in best_thread and best_thread[(graph, thread)]]
        if not pairs:
            continue
        thread_values = np.array([thread for thread, _ in pairs])
        times = np.array([time_s for _, time_s in pairs])
        if thread_values[0] == 1:
            t1 = times[0]
        else:
            t1 = times[0] * thread_values[0]
        speedup = t1 / times
        ax.plot(thread_values, speedup, marker="o", color=C_IS, linewidth=2.5,
                markersize=9, markeredgecolor="white", markeredgewidth=0.5)
        ax.plot(show_threads, show_threads, "k--", linewidth=1.0, alpha=0.4,
                label="Ideal" if index == 0 else None)
        ax.set_xscale("log", base=2)
        ax.set_yscale("log", base=2)
        ax.xaxis.set_major_formatter(ticker.ScalarFormatter())
        ax.yaxis.set_major_formatter(ticker.FuncFormatter(
            lambda value, _: f"{int(value)}" if value >= 1 else f"{value:.1f}"))
        ax.set_xticks(show_threads)
        ax.set_xlim(0.7, 280)
        ax.set_title(GRAPH_SHORT[graph])
        ax.grid(True, which="major", alpha=0.2)
        if index == 0:
            ax.set_ylabel("Parallel Speedup")
            ax.legend(loc="upper left", framealpha=0.8)

    fig.text(0.5, -0.02, "Number of Threads", ha="center")
    plt.tight_layout()
    save_svg("thread_scaling_is_only")


def plot_param_sweep(best_param):
    graph_colors = {
        "com-LiveJournal": "#001C7F",
        "com-Orkut": "#B1400D",
        "indochina-2004": "#12711C",
        "uk-2005": "#8C0800",
        "webbase-2001": "#591E71",
    }
    graphs = [graph for graph in FIGURE_GRAPHS if any(key[0] == graph for key in best_param)]
    if not graphs:
        print("  skip plot_param_sweep.svg: no parameter data")
        return

    fig, (ax_d, ax_t) = plt.subplots(2, 1, figsize=(7, 8), gridspec_kw={"hspace": 0.18})

    for graph in graphs:
        pairs = sorted((value, best_param[(graph, "CD", value)])
                       for (g, param, value) in best_param
                       if g == graph and param == "CD" and best_param[(g, param, value)])
        baseline = best_param.get((graph, "CD", 0))
        if pairs and baseline:
            xs, ys = zip(*pairs)
            ax_d.plot(xs, np.array(ys) / baseline, marker="o", color=graph_colors[graph],
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

    for graph in graphs:
        pairs = sorted((value, best_param[(graph, "SD", value)])
                       for (g, param, value) in best_param
                       if g == graph and param == "SD" and best_param[(g, param, value)])
        baseline = best_param.get((graph, "SD", 0))
        if pairs and baseline:
            xs, ys = zip(*pairs)
            ax_t.plot(xs, np.array(ys) / baseline, marker="s", color=graph_colors[graph],
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
    save_svg("plot_param_sweep")


def expected_runtime_keys():
    return {(graph, config, k)
            for graph in RUNTIME_GRAPHS
            for config in CONFIGS
            for k in range(3, 13)}


def report_missing(best_runtime, best_stats, best_thread, best_param):
    missing = []
    runtime_missing = expected_runtime_keys() - set(best_runtime)
    if runtime_missing:
        missing.append(f"runtime rows missing: {len(runtime_missing)}")
    stats_missing = {(graph, config) for graph in FIGURE_GRAPHS for config in CONFIGS} - set(best_stats)
    if stats_missing:
        missing.append(f"k=7 stats runs missing: {len(stats_missing)}")
    thread_missing = {(graph, thread) for graph in FIGURE_GRAPHS for thread in [1, 4, 16, 64, 192]} - set(best_thread)
    if thread_missing:
        missing.append(f"thread-scaling points missing: {len(thread_missing)}")
    cd_missing = {(graph, value) for graph in FIGURE_GRAPHS for value in [0, 5, 10, 15, 20, 25, 30, 40, 50]
                  if (graph, "CD", value) not in best_param}
    sd_missing = {(graph, value) for graph in FIGURE_GRAPHS for value in [0, 30, 40, 50, 55, 60, 65, 70, 75, 80, 85, 90, 95]
                  if (graph, "SD", value) not in best_param}
    if cd_missing or sd_missing:
        missing.append(f"parameter-sweep points missing: {len(cd_missing) + len(sd_missing)}")

    if missing:
        print("Incomplete data detected:")
        for item in missing:
            print(f"  - {item}")


def main():
    buckets = load_runs()
    if not buckets:
        print(f"no .out files under {OUT_DIR}")
        return

    runtime_rows, best_runtime, _ = aggregate_runtime(buckets.get("runtime", []))
    thread_rows, best_thread = aggregate_thread(buckets.get("thread", []))
    param_rows, best_param = aggregate_param(buckets.get("param", []))
    stats_rows, time_rows, density_rows, best_stats = aggregate_stats(buckets.get("stats", []))

    if runtime_rows:
        write_runtime_table(best_runtime)
        write_speedup_table(best_runtime)
    write_paper_style_csvs(runtime_rows, thread_rows, param_rows, stats_rows, time_rows, density_rows)

    print("Generating paper plots...")
    plot_runtime_tree_combined(best_stats, best_runtime)
    plot_stacked_density_combined(best_stats)
    plot_scaling_time_is_only(best_thread)
    plot_param_sweep(best_param)

    report_missing(best_runtime, best_stats, best_thread, best_param)
    print(f"results -> {RESULTS_DIR.relative_to(ROOT)}")
    print(f"tables  -> {TABLES_DIR.relative_to(ROOT)}")
    print(f"plots   -> {PLOTS_DIR.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
