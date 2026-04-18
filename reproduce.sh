#!/usr/bin/env bash
# CloverIS — single entry point to reproduce the SC26 Clover results.
#
# Usage:  ./reproduce.sh
#
# Environment variables (all optional):
#   GRAPHS    — space-separated list of graphs to run (default: all 5)
#   BINARIES  — space-separated list of binaries to run (default: all 3)
#   K_MIN     — smallest k (default: 3)
#   K_MAX     — largest k  (default: 12)
#   RUN_TIMEOUT — per-invocation timeout in seconds (default: 72h)
#
# Examples:
#   ./reproduce.sh                                          # full sweep
#   GRAPHS="com-lj" BINARIES="clover clover_is" ./reproduce.sh  # LJ only, proposed methods
#   GRAPHS="com-lj uk-2005" ./reproduce.sh                  # two graphs, all binaries

set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
GRAPHS_DIR="$HERE/graphs"
RESULTS="$HERE/results.csv"
PLOTS_DIR="$HERE/plots"
K_MIN=${K_MIN:-3}
K_MAX=${K_MAX:-12}
RUN_TIMEOUT=${RUN_TIMEOUT:-259200}  # default 72h

ALL_GRAPHS=(com-lj com-orkut indochina-2004 uk-2005 webbase-2001)
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
make -j all USE_128=1

# ── 2. Fetch + convert graphs ───────────────────────────────────────
echo "== Preparing graphs =="
mkdir -p "$GRAPHS_DIR"
cd "$GRAPHS_DIR"

SS_PRIMARY="https://suitesparse-collection-website.herokuapp.com/MM"
SS_FALLBACK="https://sparse.tamu.edu/MM"

fetch_snap() {
  local name="$1" url="$2"
  [ -f "$name.sg" ] && return
  echo "  -> $name (SNAP)"
  wget -q "$url" -O "$name.txt.gz" || curl -L -f --retry 3 -o "$name.txt.gz" "$url"
  gunzip -f "$name.txt.gz"
  sed -i '/#/d' "$name.txt"
  mv "$name.txt" "$name.el"
  "$HERE/bin/converter" -sf "$name.el" -b "$name.sg" > /dev/null
  rm -f "$name.el"
}

fetch_ss() {
  local name="$1" path="$2"
  [ -f "$name.sg" ] && return
  echo "  -> $name (SuiteSparse / LAW)"
  wget -q "$SS_PRIMARY/$path.tar.gz" -O "$name.tar.gz" \
    || wget -q "$SS_FALLBACK/$path.tar.gz" -O "$name.tar.gz" \
    || curl -L -f --retry 3 -o "$name.tar.gz" "$SS_FALLBACK/$path.tar.gz"
  tar xzf "$name.tar.gz" --strip-components=1 -C "$name" || {
    mkdir -p "$name"; tar xzf "$name.tar.gz"; mv "$name"/*/* "$name"/ 2>/dev/null || true
  }
  "$HERE/bin/converter" -sf "$name/$name.mtx" -b "$name.sg" > /dev/null
  rm -rf "$name" "$name.tar.gz"
}

for g in "${GRAPHS[@]}"; do
  case "$g" in
    com-lj)         fetch_snap com-lj    https://snap.stanford.edu/data/bigdata/communities/com-lj.ungraph.txt.gz ;;
    com-orkut)      fetch_snap com-orkut https://snap.stanford.edu/data/bigdata/communities/com-orkut.ungraph.txt.gz ;;
    indochina-2004) fetch_ss   indochina-2004 LAW/indochina-2004 ;;
    uk-2005)        fetch_ss   uk-2005        LAW/uk-2005 ;;
    webbase-2001)   fetch_ss   webbase-2001   LAW/webbase-2001 ;;
  esac
done

cd "$HERE"

# ── 3. Run benchmarks ───────────────────────────────────────────────
echo "== Running benchmarks =="

# Create results.csv with header if it doesn't exist (supports resume).
[ -f "$RESULTS" ] || echo "graph,binary,k,count,time_s" > "$RESULTS"

# Check whether a (graph, binary, k) entry already exists.
has_result() {
  local g="$1" b="$2" k="$3"
  grep -q "^${g},${b},${k}," "$RESULTS" 2>/dev/null
}

# Clover and Clover+IS accept a k range in a single invocation.
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

# Print intermediate comparison after each binary finishes,
# so reviewers see results even if the full sweep never completes.
checkpoint() {
  local label="$1"
  echo ""
  echo "== Checkpoint: $label =="
  echo "  Plotting partial results..."
  mkdir -p "$PLOTS_DIR"
  python3 plot.py "$RESULTS" "$PLOTS_DIR" || true
  if [ -f "$HERE/paper_results.csv" ]; then
    python3 "$HERE/compare.py" "$RESULTS" "$HERE/paper_results.csv" || true
  fi
}

# Run all graphs for each binary, fastest first.
# Reviewers see the full Clover+IS sweep complete before slower baselines start.
has_bin clover_is  && { echo ""; echo "  Clover+IS (proposed)"; for graph in "${GRAPHS[@]}"; do run_range "$graph" clover_is; done; checkpoint "Clover+IS complete"; }
has_bin clover     && { echo ""; echo "  Clover (cover only)"; for graph in "${GRAPHS[@]}"; do run_range "$graph" clover; done; checkpoint "Clover complete"; }
has_bin pivotscale && { echo ""; echo "  PivotScale (baseline)"; for graph in "${GRAPHS[@]}"; do run_single "$graph"; done; }

# ── 4. Verify counts ────────────────────────────────────────────────
echo "== Verifying clique counts =="
MISMATCHES=0
VERIFIED=0
TOTAL=0
for graph in "${GRAPHS[@]}"; do
  for k in $(seq $K_MIN $K_MAX); do
    counts=$(awk -F, -v g="$graph" -v k="$k" \
      '$1==g && $3==k {print $4}' "$RESULTS" | sort -u)
    nbins=$(awk -F, -v g="$graph" -v k="$k" \
      '$1==g && $3==k' "$RESULTS" | wc -l)
    [ "$nbins" -eq 0 ] && continue
    TOTAL=$((TOTAL + 1))
    n=$(echo "$counts" | wc -l)
    if [ "$n" -gt 1 ]; then
      echo "  MISMATCH: $graph k=$k counts differ: $counts"
      MISMATCHES=$((MISMATCHES + 1))
    elif [ "$nbins" -ge 2 ]; then
      VERIFIED=$((VERIFIED + 1))
    fi
  done
done
echo "  $VERIFIED/$TOTAL (graph,k) pairs verified with multi-binary agreement."
if [ "$MISMATCHES" -gt 0 ]; then
  echo "  WARNING: $MISMATCHES mismatches found."
fi

# ── 5. Plot ─────────────────────────────────────────────────────────
echo "== Plotting =="
mkdir -p "$PLOTS_DIR"
python3 plot.py "$RESULTS" "$PLOTS_DIR"

# ── 6. Compare against paper reference ────────────────────────────────
echo "== Comparing against paper_results.csv =="
if [ -f "$HERE/paper_results.csv" ]; then
  python3 "$HERE/compare.py" "$RESULTS" "$HERE/paper_results.csv"
else
  echo "  paper_results.csv not found — skipping comparison."
fi

echo ""
echo "Reproduction complete."
echo "  Numbers : $RESULTS"
echo "  Plots   : $PLOTS_DIR/"
echo ""
echo "To compare against paper reference at any time:"
echo "  python3 compare.py results.csv paper_results.csv"
