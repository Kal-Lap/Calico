#!/usr/bin/env bash
# CloverIS — single entry point to reproduce the SC26 Clover results.
#
# Usage:  ./reproduce.sh
#
# Environment variables (all optional):
#   GRAPHS      — space-separated list of graphs to run (default: all 8)
#   BINARIES    — space-separated list of binaries to run (default: all 3)
#   K_MIN       — smallest k (default: 3)
#   K_MAX       — largest k  (default: 12)
#   RUN_TIMEOUT — per-invocation timeout in seconds (default: 72h)
#   OMP_NUM_THREADS — threads for each run (default: all cores reported by nproc)
#
# Examples:
#   ./reproduce.sh
#   GRAPHS="com-LiveJournal" BINARIES="clover_is" ./reproduce.sh
#   K_MIN=7 K_MAX=7 ./reproduce.sh

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
GRAPHS_DIR="$HERE/graphs"
RESULTS="$HERE/results.csv"
PLOTS_DIR="$HERE/plots"
K_MIN=${K_MIN:-3}
K_MAX=${K_MAX:-12}
RUN_TIMEOUT=${RUN_TIMEOUT:-259200}  # default 72h
export OMP_NUM_THREADS=${OMP_NUM_THREADS:-$(nproc 2>/dev/null || echo 1)}
echo "OMP_NUM_THREADS=$OMP_NUM_THREADS"

# All eight paper graphs, all fetched from the SuiteSparse Matrix Collection.
ALL_GRAPHS=(com-LiveJournal com-Orkut com-Friendster hollywood-2009 indochina-2004 arabic-2005 uk-2005 webbase-2001)
declare -A GRAPH_PATH=(
  [com-LiveJournal]="SNAP/com-LiveJournal"
  [com-Orkut]="SNAP/com-Orkut"
  [com-Friendster]="SNAP/com-Friendster"
  [hollywood-2009]="LAW/hollywood-2009"
  [indochina-2004]="LAW/indochina-2004"
  [arabic-2005]="LAW/arabic-2005"
  [uk-2005]="LAW/uk-2005"
  [webbase-2001]="LAW/webbase-2001"
)

if [ -n "${GRAPHS:-}" ]; then
  read -ra GRAPHS <<< "$GRAPHS"
else
  GRAPHS=("${ALL_GRAPHS[@]}")
fi

ALL_BINARIES=(pivotscale clover clover_is)
if [ -n "${BINARIES:-}" ]; then
  read -ra BINARIES <<< "$BINARIES"
else
  BINARIES=("${ALL_BINARIES[@]}")
fi

# ── 1. Build ────────────────────────────────────────────────────────
echo "== Building =="
make -j all

# ── 2. Fetch + prepare graphs ───────────────────────────────────────
echo "== Preparing graphs =="
mkdir -p "$GRAPHS_DIR"
cd "$GRAPHS_DIR"

SS_PRIMARY="https://suitesparse-collection-website.herokuapp.com/MM"
SS_FALLBACK="https://sparse.tamu.edu/MM"

fetch_ss() {
  local name="$1" path="$2"
  [ -f "$name.sg" ] && return
  echo "  -> $name"
  wget -q "$SS_PRIMARY/$path.tar.gz" -O "$name.tar.gz" \
    || wget -q "$SS_FALLBACK/$path.tar.gz" -O "$name.tar.gz" \
    || curl -L -f --retry 3 -o "$name.tar.gz" "$SS_FALLBACK/$path.tar.gz"
  mkdir -p "$name"
  tar xzf "$name.tar.gz" --strip-components=1 -C "$name"
  "$HERE/bin/converter" -sf "$name/$name.mtx" -b "$name.sg" > /dev/null
  rm -rf "$name" "$name.tar.gz"
}

for g in "${GRAPHS[@]}"; do
  path="${GRAPH_PATH[$g]:-}"
  if [ -z "$path" ]; then
    echo "  [unknown graph: $g — skipping]"
    continue
  fi
  fetch_ss "$g" "$path"
done

cd "$HERE"

# ── 3. Run benchmarks ───────────────────────────────────────────────
echo "== Running benchmarks =="

[ -f "$RESULTS" ] || echo "graph,binary,k,count,time_s" > "$RESULTS"

has_result() {
  local g="$1" b="$2" k="$3"
  grep -q "^${g},${b},${k}," "$RESULTS" 2>/dev/null
}

# Clover / Clover+IS accept a k range in a single invocation.
run_range() {
  local graph="$1" bin="$2"
  local sg="$GRAPHS_DIR/$graph.sg"
  local max_done
  max_done=$(awk -F, -v g="$graph" -v b="$bin" '$1==g && $2==b {k=$3+0; if(k>m) m=k} END{print m+0}' "$RESULTS")
  local start_k=$K_MIN
  if [ "$max_done" -ge "$K_MAX" ]; then
    echo "  [$bin  $graph  already complete — skipping]"
    return
  elif [ "$max_done" -ge "$K_MIN" ]; then
    start_k=$((max_done + 1))
    echo "  [$bin  $graph  resuming at k=$start_k..$K_MAX]"
  else
    echo "  [$bin  $graph  k=$K_MIN..$K_MAX]"
  fi
  timeout "$RUN_TIMEOUT" ./bin/"$bin" -s -f "$sg" -c $start_k -l $K_MAX 2>&1 | \
    awk -v g="$graph" -v b="$bin" '
      /^Counting Time:/ { t = $NF }
      /^k:/             { print g","b","$2","$3","t }
    ' >> "$RESULTS" || echo "  [TIMEOUT after ${RUN_TIMEOUT}s]"
}

# PivotScale takes a single k; loop externally.
run_single() {
  local graph="$1"
  local sg="$GRAPHS_DIR/$graph.sg"
  for k in $(seq $K_MIN $K_MAX); do
    if has_result "$graph" pivotscale "$k"; then
      echo "  [pivotscale  $graph  k=$k  already done — skipping]"
      continue
    fi
    echo "  [pivotscale  $graph  k=$k]"
    timeout "$RUN_TIMEOUT" ./bin/pivotscale -s -f "$sg" -c "$k" 2>&1 | \
      awk -v g="$graph" -v k="$k" '
        /^Counting Time:/ { t = $NF }
        /^k:/             { print g",pivotscale,"k","$3","t }
      ' >> "$RESULTS" || echo "  [TIMEOUT after ${RUN_TIMEOUT}s]"
  done
}

has_bin() { local b="$1"; for x in "${BINARIES[@]}"; do [ "$x" = "$b" ] && return 0; done; return 1; }

# Fastest first, so partial runs still produce the proposed-method numbers.
has_bin clover_is  && { echo ""; echo "  Clover+IS (proposed)"; for graph in "${GRAPHS[@]}"; do run_range "$graph" clover_is; done; }
has_bin clover     && { echo ""; echo "  Clover (cover only)"; for graph in "${GRAPHS[@]}"; do run_range "$graph" clover; done; }
has_bin pivotscale && { echo ""; echo "  PivotScale (baseline)"; for graph in "${GRAPHS[@]}"; do run_single "$graph"; done; }

# ── 4. Plot runtime figure ──────────────────────────────────────────
echo ""
echo "== Plotting runtime =="
mkdir -p "$PLOTS_DIR"
python3 plot_runtime.py "$RESULTS" "$PLOTS_DIR" || echo "  [plotting failed — results.csv is still valid]"

echo ""
echo "Reproduction complete."
echo "  Numbers : $RESULTS"
echo "  Plots   : $PLOTS_DIR/"
echo ""
echo "Next experiments:"
echo "  scripts/thread_scaling.sh   → thread_scaling.csv → plot_thread_scaling.py"
echo "  scripts/param_sweep.sh      → param_sweep.csv    → plot_param_sweep.py"
