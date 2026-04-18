# CloverIS

Reproducibility artifact for the SC26 submission introducing **Clover**, a
parallel k-clique counting algorithm, and **Clover+IS**, its extension that
counts independent sets on the complement graph.

## One-command reproduction

```
./reproduce.sh
```

The script builds the three counting binaries plus a graph converter,
downloads and converts the input graphs from public sources, runs the
benchmark sweep (k = 3..12), writes per-run times to `results.csv`, and
produces the runtime figure in `plots/`.

Compare the `time_s` column of `results.csv` against `paper_results.csv`
(shipped in this repo) to verify reproduction. A comparison script is
provided:

```
python3 compare.py results.csv paper_results.csv
```

This checks coverage (all paper entries present) and count agreement
across binaries.

### Selective runs

The script accepts environment variables to control scope:

```bash
# Only the proposed algorithms on com-LiveJournal (under 1 minute):
GRAPHS="com-lj" BINARIES="clover clover_is" ./reproduce.sh

# All binaries on two graphs:
GRAPHS="com-lj uk-2005" ./reproduce.sh

# Full sweep (all 5 graphs × 3 binaries × k=3..12):
./reproduce.sh
```

The script is resumable: completed entries are skipped on re-run.
To start fresh, delete `results.csv` first.

## Requirements

- Linux x86-64
- gcc 11 or newer (C++20 with OpenMP)
- Python 3.8+ with matplotlib
- `curl`, `tar`, `gunzip`
- Disk: ~40 GB for the converted `.sg` graphs (webbase-2001 dominates)
- RAM: ~128 GB recommended for webbase-2001; smaller graphs run on a laptop

The artifact has been run end-to-end on an Intel Granite Rapids node
(192 threads, 768 GB RAM). Any x86-64 Linux box with enough memory will
reproduce the same trends; absolute timings will differ.

## Graphs

| Graph          | Source                | |V|      | |E|     |
| -------------- | --------------------- | -------- | ------- |
| com-lj         | SNAP                  | 4.0 M    | 34.7 M  |
| com-orkut      | SNAP                  | 3.1 M    | 117.2 M |
| indochina-2004 | SuiteSparse (LAW)     | 7.4 M    | 194.1 M |
| uk-2005        | SuiteSparse (LAW)     | 39.5 M   | 936.4 M |
| webbase-2001   | SuiteSparse (LAW)     | 118.1 M  | 1.02 B  |

## Layout

```
CloverIS/
├── reproduce.sh       single entry point
├── Makefile           builds the four binaries into bin/
├── plot.py            produces plots/runtime.{png,svg}
├── src/
│   ├── clover/        Clover and Clover+IS (ENABLE_IS flag)
│   └── pivotscale/    PivotScale baseline + graph converter
├── paper_results.csv  authoritative numbers from our runs
└── paper/             AD/AE appendix LaTeX source
```

## Runtime

**Clover+IS** is orders of magnitude faster than PivotScale — seconds on
com-lj/com-orkut/indochina, minutes on uk-2005, hours on webbase-2001
at high k. PivotScale takes hours to days on the larger graphs. The full
sweep including all baselines takes several days on a 192-thread node.

For a quick functional check, run only the proposed algorithms on
com-LiveJournal — this completes in under one minute and demonstrates the
key speedup (Clover+IS k=12 in ~10s vs PivotScale k=12 in ~35 hours):

```
GRAPHS="com-lj" BINARIES="clover clover_is" ./reproduce.sh
```

## License

See `LICENSE`.
