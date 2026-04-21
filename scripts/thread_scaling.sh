#!/usr/bin/env bash
# Thread scaling for Clover+IS at fixed k (paper figure: parallel speedup).
# Run ./reproduce.sh first so the .sg files exist in graphs/.
#
# Environment variables:
#   GRAPHS   — space-separated (default: 5 representative graphs)
#   THREADS  — space-separated (default: 1 2 4 8 16 32 64 128 192)
#   K        — clique size (default: 7)

set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
RESULTS="$HERE/thread_scaling.csv"
K=${K:-7}
GRAPHS=${GRAPHS:-"com-LiveJournal com-Orkut indochina-2004 uk-2005 webbase-2001"}
THREADS=${THREADS:-"1 2 4 8 16 32 64 128 192"}

[ -f "$RESULTS" ] || echo "graph,threads,k,count,time_s" > "$RESULTS"

read -ra GRAPHS_ARR <<< "$GRAPHS"
read -ra THREADS_ARR <<< "$THREADS"

for graph in "${GRAPHS_ARR[@]}"; do
  sg="$HERE/graphs/$graph.sg"
  if [ ! -f "$sg" ]; then
    echo "  [skip $graph — $sg missing; run ./reproduce.sh first]"
    continue
  fi
  for t in "${THREADS_ARR[@]}"; do
    if grep -q "^${graph},${t},${K}," "$RESULTS" 2>/dev/null; then
      echo "  [$graph  t=$t  k=$K  already done]"
      continue
    fi
    echo "  [$graph  t=$t  k=$K]"
    OMP_NUM_THREADS="$t" "$HERE/bin/clover_is" -s -f "$sg" -c "$K" -l "$K" 2>&1 | \
      awk -v g="$graph" -v t="$t" '
        /^Counting Time:/ { tm = $NF }
        /^k:/             { print g","t","$2","$3","tm }
      ' >> "$RESULTS"
  done
done

echo "Done. Plot with:  python3 plot_thread_scaling.py $RESULTS plots/"
