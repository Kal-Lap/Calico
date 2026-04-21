# CloverIS

Reproducibility artifact for the SC26 submission introducing **Clover**, a
parallel k-clique counting algorithm, and **Clover+IS**, its extension that
counts independent sets on the complement graph.

## Quick start

```bash
python3 -m pip install -r requirements.txt
./reproduce.sh
```

`reproduce.sh` builds the three counting binaries plus a graph converter,
downloads the eight input graphs from the SuiteSparse Matrix Collection,
runs the benchmark sweep (k = 3..12), writes per-run times to
`results.csv`, and renders the runtime figure in `plots/`.

For a smaller end-to-end run (Clover+IS on com-LiveJournal at k=7):

```bash
make run-example
```

## Selective runs

All scopes come from env vars; the default is the full sweep.

```bash
# Clover+IS only on com-LiveJournal
GRAPHS="com-LiveJournal" BINARIES="clover_is" ./reproduce.sh

# Runtime table at k=7 only
K_MIN=7 K_MAX=7 ./reproduce.sh

# Two graphs, all binaries, k=3..12
GRAPHS="com-LiveJournal uk-2005" ./reproduce.sh
```

The script is resumable: already-completed `(graph, binary, k)` rows are
skipped on re-run. Delete `results.csv` to start fresh.

## Thread scaling and parameter sweep

Two additional experiments live in `scripts/`. They reuse the `.sg`
files produced by `reproduce.sh`, so run `reproduce.sh` first (or at least
let the fetch phase complete).

```bash
# Parallel speedup vs thread count, Clover+IS, k=7
scripts/thread_scaling.sh
python3 plot_thread_scaling.py thread_scaling.csv plots/

# CD (complement-degree) and SD (subgraph-density) parameter sweep, k=7
scripts/param_sweep.sh
python3 plot_param_sweep.py param_sweep.csv plots/
```

Both scripts accept the same kinds of env-var overrides as `reproduce.sh`
(`GRAPHS`, `K`, `THREADS`, `CD_VALUES`, `SD_VALUES`).

## Requirements

- Linux x86-64
- gcc 11 or newer (C++20 with OpenMP)
- Python 3.8+ with matplotlib (see `requirements.txt`)
- `curl`, `tar`, `gunzip`
- Disk: ~60 GB for the converted `.sg` files (webbase and com-Friendster dominate)
- RAM: ≥128 GB recommended for the large web graphs; smaller graphs
  run on a laptop

The artifact has been run end-to-end on an Intel Granite Rapids node
(192 threads, 768 GB RAM). Any x86-64 Linux box with enough memory will
reproduce the same trends; absolute timings will differ.

`reproduce.sh` sets `OMP_NUM_THREADS` to `$(nproc)` if unset. Override as
needed:

```bash
OMP_NUM_THREADS=64 ./reproduce.sh
```

If your environment picks a compiler without OpenMP support, rebuild with:

```bash
make CXX=g++ -j all
```

## Graphs

All eight graphs are fetched from the SuiteSparse Matrix Collection at
runtime.

| Graph            | Source                                          |
| ---------------- | ----------------------------------------------- |
| com-LiveJournal  | SuiteSparse / SNAP                              |
| com-Orkut        | SuiteSparse / SNAP                              |
| com-Friendster   | SuiteSparse / SNAP                              |
| hollywood-2009   | SuiteSparse / LAW                               |
| indochina-2004   | SuiteSparse / LAW                               |
| arabic-2005      | SuiteSparse / LAW                               |
| uk-2005          | SuiteSparse / LAW                               |
| webbase-2001     | SuiteSparse / LAW                               |

## Layout

```
CloverIS/
├── reproduce.sh               main benchmark sweep → results.csv
├── Makefile                   builds bin/{clover, clover_is, pivotscale, converter}
├── requirements.txt           Python plotting dependency
├── plot_runtime.py            results.csv          → plots/runtime.{png,svg}
├── plot_thread_scaling.py     thread_scaling.csv   → plots/thread_scaling.{png,svg}
├── plot_param_sweep.py        param_sweep.csv      → plots/param_sweep.{png,svg}
├── scripts/
│   ├── thread_scaling.sh      Clover+IS × {1..192} threads at k=7
│   └── param_sweep.sh         Clover+IS × CD and SD sweeps at k=7
└── src/
    ├── clover/                Clover and Clover+IS (ENABLE_IS compile flag)
    └── pivotscale/            PivotScale baseline + graph converter
```

## License

See `LICENSE`.
