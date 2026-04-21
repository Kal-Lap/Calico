#!/usr/bin/env bash
# Submit the SC26 CloverIS artifact jobs and exit.
#
# Reviewer workflow:
#   1. ./reproduce.sh
#   2. python3 generate_paper_outputs.py
#
# Structure: build -> fetch -> {runtime, thread_t$N, param, stats} arrays.

set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$HERE"

if [[ -f "$HERE/.env" ]]; then
  # shellcheck disable=SC1091
  source "$HERE/.env"
fi

JOBS_DIR=${JOBS_DIR:-"$HERE/jobs"}
OUT_DIR=${OUT_DIR:-"$HERE/output"}
GRAPHS_DIR=${GRAPHS_DIR:-"$HERE/graphs"}

SCRATCH_ROOT=${SCRATCH_ROOT:-}

RUNTIME_GRAPHS=${RUNTIME_GRAPHS:-"com-LiveJournal com-Orkut com-Friendster hollywood-2009 indochina-2004 arabic-2005 uk-2005 webbase-2001"}
FIGURE_GRAPHS=${FIGURE_GRAPHS:-"com-LiveJournal com-Orkut indochina-2004 uk-2005 webbase-2001"}
CONFIGS=${CONFIGS:-"pivotscale clover clover_is"}

K_MIN=${K_MIN:-3}
K_MAX=${K_MAX:-12}
K_FIGURE=${K_FIGURE:-7}
REPEATS=${REPEATS:-3}
STATS_REPEATS=${STATS_REPEATS:-1}

THREADS=${THREADS:-192}
THREAD_SWEEP=${THREAD_SWEEP:-"1 4 16 64 192"}
CD_VALUES=${CD_VALUES:-"0 5 10 15 20 25 30 40 50"}
SD_VALUES=${SD_VALUES:-"0 30 40 50 55 60 65 70 75 80 85 90 95"}

SLURM_ACCOUNT=${SLURM_ACCOUNT:-}
SLURM_PARTITION=${SLURM_PARTITION:-}
SLURM_CONSTRAINT=${SLURM_CONSTRAINT:-}

BUILD_CPUS=${BUILD_CPUS:-16}
BUILD_TIME=${BUILD_TIME:-01:00:00}
FETCH_TIME=${FETCH_TIME:-12:00:00}
RUN_TIME=${RUN_TIME:-72:00:00}
SWEEP_TIME=${SWEEP_TIME:-72:00:00}
STATS_TIME=${STATS_TIME:-72:00:00}
DRY_RUN=${DRY_RUN:-0}

SBATCH_ACCOUNT_LINE=${SLURM_ACCOUNT:+#SBATCH -A $SLURM_ACCOUNT}
SBATCH_PARTITION_LINE=${SLURM_PARTITION:+#SBATCH -p $SLURM_PARTITION}
SBATCH_CONSTRAINT_LINE=${SLURM_CONSTRAINT:+#SBATCH --constraint=$SLURM_CONSTRAINT}

if [[ -n "$SCRATCH_ROOT" ]]; then
  mkdir -p "$SCRATCH_ROOT/output" "$SCRATCH_ROOT/graphs"
  for name in output graphs; do
    target="$SCRATCH_ROOT/$name"
    link="$HERE/$name"
    if [[ -L "$link" ]]; then
      ln -sfn "$target" "$link"
    elif [[ -e "$link" ]]; then
      echo "$link exists and is not a symlink; move or remove it before setting SCRATCH_ROOT" >&2
      exit 1
    else
      ln -s "$target" "$link"
    fi
  done
  OUT_DIR="$HERE/output"
  GRAPHS_DIR="$HERE/graphs"
fi

mkdir -p "$JOBS_DIR" "$OUT_DIR"/{build,fetch,runtime,thread,param,stats} "$GRAPHS_DIR"

require_sbatch() {
  if [[ "$DRY_RUN" == "1" ]]; then return; fi
  if ! command -v sbatch >/dev/null 2>&1; then
    echo "sbatch not found. Run on the cluster login node, or use DRY_RUN=1 to only generate job files." >&2
    exit 1
  fi
}

submit_job() {
  local script="$1"
  local dependency="${2:-}"
  if [[ "$DRY_RUN" == "1" ]]; then
    if [[ -n "$dependency" ]]; then
      echo "DRY_RUN: sbatch --dependency=$dependency $script" >&2
    else
      echo "DRY_RUN: sbatch $script" >&2
    fi
    echo "DRYRUN"
    return
  fi

  local output attempt=0 max_attempts=6 rc=0
  while (( attempt < max_attempts )); do
    if [[ -n "$dependency" ]]; then
      output=$(sbatch --dependency="$dependency" "$script" 2>&1); rc=$?
    else
      output=$(sbatch "$script" 2>&1); rc=$?
    fi
    if (( rc == 0 )); then break; fi
    attempt=$((attempt + 1))
    echo "sbatch retry $attempt/$max_attempts for $(basename "$script"): $output" >&2
    sleep $((attempt * 5))
  done
  if (( rc != 0 )); then
    echo "sbatch failed after $max_attempts attempts: $script" >&2
    exit 1
  fi
  echo "$output" >&2
  awk '{print $4}' <<< "$output"
}

dependency_afterok() {
  local jid="$1"
  if [[ "$DRY_RUN" == "1" || -z "$jid" || "$jid" == "DRYRUN" ]]; then
    echo ""
  else
    echo "afterok:$jid"
  fi
}

write_build_job() {
  cat > "$JOBS_DIR/00_build.sbatch" <<EOF
#!/usr/bin/env bash
#SBATCH -J clover_build
$SBATCH_ACCOUNT_LINE
$SBATCH_PARTITION_LINE
$SBATCH_CONSTRAINT_LINE
#SBATCH -N 1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=$BUILD_CPUS
#SBATCH --time=$BUILD_TIME
#SBATCH --output=output/build/%x-%j.out

set -euo pipefail
cd "$HERE"
echo "RUN kind=build threads=$BUILD_CPUS"
echo "HOST \$(hostname)"
echo "START \$(date -Is)"
make -j "$BUILD_CPUS" all
echo "END \$(date -Is)"
EOF
}

write_fetch_job() {
  cat > "$JOBS_DIR/01_fetch_graphs.sbatch" <<EOF
#!/usr/bin/env bash
#SBATCH -J clover_fetch
$SBATCH_ACCOUNT_LINE
$SBATCH_PARTITION_LINE
$SBATCH_CONSTRAINT_LINE
#SBATCH -N 1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=$BUILD_CPUS
#SBATCH --time=$FETCH_TIME
#SBATCH --output=output/fetch/%x-%j.out

set -euo pipefail
cd "$HERE"
echo "RUN kind=fetch threads=$BUILD_CPUS"
echo "HOST \$(hostname)"
echo "START \$(date -Is)"
mkdir -p "$GRAPHS_DIR"

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

SS_PRIMARY="https://suitesparse-collection-website.herokuapp.com/MM"
SS_FALLBACK="https://sparse.tamu.edu/MM"
GRAPHS=($RUNTIME_GRAPHS $FIGURE_GRAPHS)

cd "$GRAPHS_DIR"
for graph in "\${GRAPHS[@]}"; do
  path="\${GRAPH_PATH[\$graph]:-}"
  if [[ -z "\$path" ]]; then
    echo "unknown graph: \$graph" >&2
    exit 2
  fi
  if [[ -f "\$graph.sg" ]]; then
    echo "FETCH graph=\$graph status=exists"
    continue
  fi
  echo "FETCH graph=\$graph status=download"
  wget -q "\$SS_PRIMARY/\$path.tar.gz" -O "\$graph.tar.gz" \\
    || wget -q "\$SS_FALLBACK/\$path.tar.gz" -O "\$graph.tar.gz" \\
    || curl -L -f --retry 3 -o "\$graph.tar.gz" "\$SS_FALLBACK/\$path.tar.gz"
  mkdir -p "\$graph"
  tar xzf "\$graph.tar.gz" --strip-components=1 -C "\$graph"
  "$HERE/bin/converter" -sf "\$graph/\$graph.mtx" -b "\$graph.sg" >/dev/null
  rm -rf "\$graph" "\$graph.tar.gz"
  echo "FETCH graph=\$graph status=converted"
done

echo "END \$(date -Is)"
EOF
}

build_runtime_tasks() {
  local manifest="$JOBS_DIR/runtime_tasks.txt"
  : > "$manifest"
  for graph in $RUNTIME_GRAPHS; do
    for config in $CONFIGS; do
      for ((k = K_MIN; k <= K_MAX; k++)); do
        for ((repeat = 1; repeat <= REPEATS; repeat++)); do
          echo "$graph $config $k $repeat" >> "$manifest"
        done
      done
    done
  done
  wc -l < "$manifest" | tr -d ' '
}

build_param_tasks() {
  local manifest="$JOBS_DIR/param_tasks.txt"
  : > "$manifest"
  for graph in $FIGURE_GRAPHS; do
    for v in $CD_VALUES; do
      for ((repeat = 1; repeat <= REPEATS; repeat++)); do
        echo "$graph CD $v $repeat" >> "$manifest"
      done
    done
    for v in $SD_VALUES; do
      for ((repeat = 1; repeat <= REPEATS; repeat++)); do
        echo "$graph SD $v $repeat" >> "$manifest"
      done
    done
  done
  wc -l < "$manifest" | tr -d ' '
}

build_stats_tasks() {
  local manifest="$JOBS_DIR/stats_tasks.txt"
  : > "$manifest"
  for graph in $FIGURE_GRAPHS; do
    for config in $CONFIGS; do
      for ((repeat = 1; repeat <= STATS_REPEATS; repeat++)); do
        echo "$graph $config $repeat" >> "$manifest"
      done
    done
  done
  wc -l < "$manifest" | tr -d ' '
}

build_thread_tasks() {
  local threads="$1"
  local manifest="$JOBS_DIR/thread_t${threads}_tasks.txt"
  : > "$manifest"
  for graph in $FIGURE_GRAPHS; do
    for ((repeat = 1; repeat <= REPEATS; repeat++)); do
      echo "$graph $threads $repeat" >> "$manifest"
    done
  done
  wc -l < "$manifest" | tr -d ' '
}

write_runtime_array() {
  local count="$1"
  cat > "$JOBS_DIR/runtime.sbatch" <<EOF
#!/usr/bin/env bash
#SBATCH -J rt
$SBATCH_ACCOUNT_LINE
$SBATCH_PARTITION_LINE
$SBATCH_CONSTRAINT_LINE
#SBATCH -N 1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=$THREADS
#SBATCH --time=$RUN_TIME
#SBATCH --array=1-$count
#SBATCH --output=output/runtime/%A_%a.out

set -euo pipefail
cd "$HERE"
IFS=" " read -r GRAPH CONFIG K REPEAT < <(sed -n "\${SLURM_ARRAY_TASK_ID}p" "$JOBS_DIR/runtime_tasks.txt")
case "\$CONFIG" in
  pivotscale) BIN=bin/pivotscale; EXTRA="" ;;
  clover)     BIN=bin/clover;     EXTRA="-l \$K" ;;
  clover_is)  BIN=bin/clover_is;  EXTRA="-l \$K" ;;
  *) echo "unknown config: \$CONFIG" >&2; exit 1 ;;
esac
export OMP_NUM_THREADS=$THREADS
export OMP_PLACES=cores
export OMP_PROC_BIND=close
export OMP_STACKSIZE=16M
echo "RUN kind=runtime graph=\$GRAPH config=\$CONFIG k=\$K repeat=\$REPEAT threads=$THREADS"
echo "HOST \$(hostname)"
echo "START \$(date -Is)"
if command -v stdbuf >/dev/null 2>&1; then
  stdbuf -oL ./\$BIN -s -f "graphs/\$GRAPH.sg" -c \$K \$EXTRA
else
  ./\$BIN -s -f "graphs/\$GRAPH.sg" -c \$K \$EXTRA
fi
echo "END \$(date -Is)"
EOF
}

write_thread_array() {
  local threads="$1"
  local count="$2"
  cat > "$JOBS_DIR/thread_t${threads}.sbatch" <<EOF
#!/usr/bin/env bash
#SBATCH -J th_t${threads}
$SBATCH_ACCOUNT_LINE
$SBATCH_PARTITION_LINE
$SBATCH_CONSTRAINT_LINE
#SBATCH -N 1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=$threads
#SBATCH --time=$SWEEP_TIME
#SBATCH --array=1-$count
#SBATCH --output=output/thread/%A_%a.out

set -euo pipefail
cd "$HERE"
IFS=" " read -r GRAPH THR REPEAT < <(sed -n "\${SLURM_ARRAY_TASK_ID}p" "$JOBS_DIR/thread_t${threads}_tasks.txt")
export OMP_NUM_THREADS=\$THR
export OMP_PLACES=cores
export OMP_PROC_BIND=close
export OMP_STACKSIZE=16M
echo "RUN kind=thread graph=\$GRAPH config=clover_is k=$K_FIGURE repeat=\$REPEAT threads=\$THR"
echo "HOST \$(hostname)"
echo "START \$(date -Is)"
if command -v stdbuf >/dev/null 2>&1; then
  stdbuf -oL ./bin/clover_is -s -f "graphs/\$GRAPH.sg" -c $K_FIGURE -l $K_FIGURE
else
  ./bin/clover_is -s -f "graphs/\$GRAPH.sg" -c $K_FIGURE -l $K_FIGURE
fi
echo "END \$(date -Is)"
EOF
}

write_param_array() {
  local count="$1"
  cat > "$JOBS_DIR/param.sbatch" <<EOF
#!/usr/bin/env bash
#SBATCH -J pm
$SBATCH_ACCOUNT_LINE
$SBATCH_PARTITION_LINE
$SBATCH_CONSTRAINT_LINE
#SBATCH -N 1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=$THREADS
#SBATCH --time=$SWEEP_TIME
#SBATCH --array=1-$count
#SBATCH --output=output/param/%A_%a.out

set -euo pipefail
cd "$HERE"
IFS=" " read -r GRAPH PARAM VALUE REPEAT < <(sed -n "\${SLURM_ARRAY_TASK_ID}p" "$JOBS_DIR/param_tasks.txt")
if [[ "\$PARAM" == "CD" ]]; then
  export IS_D=\$VALUE
  export IS_T=0
else
  export IS_D=20
  export IS_T=\$VALUE
fi
export OMP_NUM_THREADS=$THREADS
export OMP_PLACES=cores
export OMP_PROC_BIND=close
export OMP_STACKSIZE=16M
echo "RUN kind=param graph=\$GRAPH config=clover_is k=$K_FIGURE param=\$PARAM value=\$VALUE repeat=\$REPEAT threads=$THREADS"
echo "HOST \$(hostname)"
echo "START \$(date -Is)"
if command -v stdbuf >/dev/null 2>&1; then
  stdbuf -oL ./bin/clover_is -s -f "graphs/\$GRAPH.sg" -c $K_FIGURE -l $K_FIGURE
else
  ./bin/clover_is -s -f "graphs/\$GRAPH.sg" -c $K_FIGURE -l $K_FIGURE
fi
echo "END \$(date -Is)"
EOF
}

write_stats_array() {
  local count="$1"
  cat > "$JOBS_DIR/stats.sbatch" <<EOF
#!/usr/bin/env bash
#SBATCH -J st
$SBATCH_ACCOUNT_LINE
$SBATCH_PARTITION_LINE
$SBATCH_CONSTRAINT_LINE
#SBATCH -N 1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=$THREADS
#SBATCH --time=$STATS_TIME
#SBATCH --array=1-$count
#SBATCH --output=output/stats/%A_%a.out

set -euo pipefail
cd "$HERE"
IFS=" " read -r GRAPH CONFIG REPEAT < <(sed -n "\${SLURM_ARRAY_TASK_ID}p" "$JOBS_DIR/stats_tasks.txt")
case "\$CONFIG" in
  pivotscale) BIN=bin/pivotscale_stats; EXTRA="" ;;
  clover)     BIN=bin/clover_stats;     EXTRA="-l $K_FIGURE" ;;
  clover_is)  BIN=bin/clover_is_stats;  EXTRA="-l $K_FIGURE" ;;
  *) echo "unknown config: \$CONFIG" >&2; exit 1 ;;
esac
export OMP_NUM_THREADS=$THREADS
export OMP_PLACES=cores
export OMP_PROC_BIND=close
export OMP_STACKSIZE=16M
echo "RUN kind=stats graph=\$GRAPH config=\$CONFIG k=$K_FIGURE repeat=\$REPEAT threads=$THREADS"
echo "HOST \$(hostname)"
echo "START \$(date -Is)"
if command -v stdbuf >/dev/null 2>&1; then
  stdbuf -oL ./\$BIN -s -f "graphs/\$GRAPH.sg" -c $K_FIGURE \$EXTRA
else
  ./\$BIN -s -f "graphs/\$GRAPH.sg" -c $K_FIGURE \$EXTRA
fi
echo "END \$(date -Is)"
EOF
}

require_sbatch

write_build_job
write_fetch_job

build_jid=$(submit_job "$JOBS_DIR/00_build.sbatch")
build_dep=$(dependency_afterok "$build_jid")
fetch_jid=$(submit_job "$JOBS_DIR/01_fetch_graphs.sbatch" "$build_dep")
fetch_dep=$(dependency_afterok "$fetch_jid")

runtime_count=$(build_runtime_tasks)
param_count=$(build_param_tasks)
stats_count=$(build_stats_tasks)

write_runtime_array "$runtime_count"
write_param_array "$param_count"
write_stats_array "$stats_count"

runtime_jid=$(submit_job "$JOBS_DIR/runtime.sbatch" "$fetch_dep")
param_jid=$(submit_job "$JOBS_DIR/param.sbatch" "$fetch_dep")
stats_jid=$(submit_job "$JOBS_DIR/stats.sbatch" "$fetch_dep")

thread_summary=""
for t in $THREAD_SWEEP; do
  thread_count=$(build_thread_tasks "$t")
  write_thread_array "$t" "$thread_count"
  jid=$(submit_job "$JOBS_DIR/thread_t${t}.sbatch" "$fetch_dep")
  thread_summary="$thread_summary t=$t:$jid($thread_count)"
done

echo ""
echo "Submitted CloverIS artifact workflow."
echo "  build   : $build_jid"
echo "  fetch   : $fetch_jid"
echo "  runtime : $runtime_jid [$runtime_count tasks]"
echo "  param   : $param_jid [$param_count tasks]"
echo "  stats   : $stats_jid [$stats_count tasks]"
echo "  thread  :$thread_summary"
echo "  jobs    : $JOBS_DIR"
echo "  output  : $OUT_DIR"
echo ""
echo "After jobs finish, run:"
echo "  python3 generate_paper_outputs.py"
