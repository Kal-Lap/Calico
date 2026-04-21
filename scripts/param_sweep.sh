#!/usr/bin/env bash
# CD / SD parameter sweep for Clover+IS (paper figure: param sweep).
# Run ./reproduce.sh first so the .sg files exist in graphs/.
#
# Top panel: vary CD (IS_D) with SD fixed at 0.
# Bottom panel: vary SD (IS_T) with CD fixed at 20.
#
# Environment variables:
#   GRAPHS     — space-separated (default: 5 representative graphs)
#   CD_VALUES  — values to sweep for CD (default: 0 5 10 15 20 25 30 40 50)
#   SD_VALUES  — values to sweep for SD (default: 0 25 35 45 55 65 75 85)
#   K          — clique size (default: 7)

set -euo pipefail

HERE=$(cd "$(dirname "$0")/.." && pwd)
RESULTS="$HERE/param_sweep.csv"
K=${K:-7}
GRAPHS=${GRAPHS:-"com-LiveJournal com-Orkut indochina-2004 uk-2005 webbase-2001"}
CD_VALUES=${CD_VALUES:-"0 5 10 15 20 25 30 40 50"}
SD_VALUES=${SD_VALUES:-"0 25 35 45 55 65 75 85"}

[ -f "$RESULTS" ] || echo "graph,param,value,k,count,time_s" > "$RESULTS"

read -ra GRAPHS_ARR <<< "$GRAPHS"
read -ra CD_ARR <<< "$CD_VALUES"
read -ra SD_ARR <<< "$SD_VALUES"

run_one() {
  local graph="$1" param="$2" value="$3"
  local sg="$HERE/graphs/$graph.sg"
  if [ ! -f "$sg" ]; then
    echo "  [skip $graph — $sg missing; run ./reproduce.sh first]"
    return
  fi
  if grep -q "^${graph},${param},${value},${K}," "$RESULTS" 2>/dev/null; then
    echo "  [$graph  $param=$value  already done]"
    return
  fi
  echo "  [$graph  $param=$value  k=$K]"
  local is_d is_t
  case "$param" in
    CD) is_d="$value"; is_t=0 ;;
    SD) is_d=20; is_t="$value" ;;
  esac
  IS_D="$is_d" IS_T="$is_t" "$HERE/bin/clover_is" -s -f "$sg" -c "$K" -l "$K" 2>&1 | \
    awk -v g="$graph" -v p="$param" -v v="$value" '
      /^Counting Time:/ { tm = $NF }
      /^k:/             { print g","p","v","$2","$3","tm }
    ' >> "$RESULTS"
}

for graph in "${GRAPHS_ARR[@]}"; do
  for cd in "${CD_ARR[@]}"; do run_one "$graph" CD "$cd"; done
  for sd in "${SD_ARR[@]}"; do run_one "$graph" SD "$sd"; done
done

echo "Done. Plot with:  python3 plot_param_sweep.py $RESULTS plots/"
