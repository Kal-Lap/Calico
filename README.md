# CloverIS

Reproducibility artifact for the SC26 submission introducing **Clover**, a
parallel k-clique counting algorithm (contribution $C_1$), and **Clover+IS**, its extension that counts independent sets on the complement graph (Hybrid algorithm of contribution $C_1$ and $C_2$).

## Building & Executing on Many-Core system
The artifact requires a single many-core system with $\geq 128$GB RAM and $\geq 60$GB disk. See software requirements below. 

To compile all executables for reproducing experimental results:
```bash
cd /path/to/source/repo
make -j all
```

To compile only the implementation of $C_1$:
```bash
cd /path/to/source/repo
make bin/clover
```

To compile only the implementation of the hybrid algorithm of $C_1$ and $C_2$:
```bash
cd /path/to/source/repo
make bin/clover_is
```

To run:
```bash
OMP_NUM_THREADS=<threads> ./<executable> -s -f "path/to/<input_graph>.sg" -c "<clique_size>" -l "<clique_size>"
```

For example, executing the hybrid algorithm of $C_1$ and $C_2$ on the graph com-LiveJournal to count cliques of size 7 using 128 threads:
```bash
OMP_NUM_THREADS=128 ./bin/clover_is -s -f "graphs/com-LiveJournal.sg" -c "7" -l "7"
```

To convert an input graph with .mtx format into .sg format, compile the converter and run:
```bash
cd /path/to/source/repo
make bin/converter
./bin/converter -sf "path/to/<input_graph>.mtx" -b "<input_graph>.sg"
```


## Experiment Quick start
Our artifacts do not require SLURM to build or execute. However, to reproduce the experimental result, we recommend submitting jobs via SLURM scripts. We provide \texttt{reproduce.sh} to generate all SLURM scripts nescessary to prepare all input data and submit experimental runs.

```bash
# 1. Set SLURM scope for your cluster (any of these can be omitted;
#    the corresponding #SBATCH line is then dropped and SLURM's defaults
#    are used).
export SLURM_ACCOUNT=my-account
export SLURM_PARTITION=my-queue
export SLURM_CONSTRAINT=my-arch

# 2. Submit the workflow:
./reproduce.sh

# 3. When all jobs finish, generate paper outputs:
python3 -m pip install -r requirements.txt
python3 generate_paper_outputs.py
```

`reproduce.sh` writes sbatch scripts under `jobs/` and submits them with
explicit dependencies:

```
00_build.sbatch  →  01_fetch_graphs.sbatch  →  {runtime, thread, param, stats}
```

The build job compiles every binary on the compute node (needed so
`-march=native` targets the actual hardware). The fetch job downloads the
eight SuiteSparse graphs and converts them to `.sg`. All experiment jobs
wait for `afterok` on the fetch job, so nothing runs until the inputs are
ready.

`generate_paper_outputs.py` scans `output/**/*.out`, aggregates per
experiment kind, and emits:

- `results/runtime.csv`, `results/thread_scaling_k7.csv`,
  `results/param_sweep_k7.csv`, `results/summary_k7.csv`,
  `results/stats_*.csv` — machine-readable data
- `tables/runtime.tex`, `tables/speedup.tex` — paper tables
- `plots/plot_runtime_tree.svg`, `plots/plot_stacked_density.svg`,
  `plots/thread_scaling_is_only.svg`, `plots/plot_param_sweep.svg` —
  paper figures

## Selective runs

Every scope is controlled by an environment variable. Defaults reproduce
the full paper sweep.

```bash
# Smoke test: only com-LiveJournal, clover_is, k=7, one repeat
RUNTIME_GRAPHS="com-LiveJournal" FIGURE_GRAPHS="com-LiveJournal" \
  CONFIGS="clover_is" K_MIN=7 K_MAX=7 REPEATS=1 ./reproduce.sh

# Generate only the sbatch files without submitting
DRY_RUN=1 ./reproduce.sh
```

Submission is idempotent at the job-file level (a rerun overwrites
`jobs/*.sbatch` and resubmits). Outputs accumulate under `output/`; delete
the directory to start fresh.

## Software Requirements

- Linux x86-64 (SLURM optional — only used by `reproduce.sh` for automated
  reproduction; building and running the binaries directly needs no SLURM)
- gcc 11 or newer (C++20 with OpenMP)
- Python 3.8+ with matplotlib, numpy, pandas, and seaborn (see
  `requirements.txt`)
- `wget` or `curl`, `tar`
- Disk: ~60 GB for the converted `.sg` files (webbase and com-Friendster
  dominate)
- RAM: ≥128 GB recommended for the large web graphs

The paper numbers were produced on an Intel Xeon 6972P (Granite Rapids,
192 cores, 768 GB RAM). Any x86-64 Linux cluster with enough memory per
node will reproduce the same trends; absolute timings will differ.

## Graphs

All eight graphs are fetched from the SuiteSparse Matrix Collection at
runtime.

| Graph            | Source              |
| ---------------- | ------------------- |
| com-LiveJournal  | SuiteSparse / SNAP  |
| com-Orkut        | SuiteSparse / SNAP  |
| com-Friendster   | SuiteSparse / SNAP  |
| hollywood-2009   | SuiteSparse / LAW   |
| indochina-2004   | SuiteSparse / LAW   |
| arabic-2005      | SuiteSparse / LAW   |
| uk-2005          | SuiteSparse / LAW   |
| webbase-2001     | SuiteSparse / LAW   |

## Layout

```
CloverIS/
├── reproduce.sh                 writes jobs/*.sbatch and submits them
├── generate_paper_outputs.py    parses output/**/*.out → tables + figures
├── Makefile                     6 binaries from 2 source files
├── requirements.txt             matplotlib, numpy, pandas, seaborn
└── src/
    ├── clover/                  Clover and Clover+IS (ENABLE_IS, ENABLE_STATS)
    └── pivotscale/              PivotScale baseline + graph converter
```

## License

See `LICENSE`.
