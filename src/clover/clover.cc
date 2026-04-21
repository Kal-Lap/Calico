// Copyright (c) 2025, The Regents of the University of California (Regents)
// See LICENSE for license details

#include "pivotscale.h"
#if ENABLE_IS
#include "complement_count.h"
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <omp.h>

#ifndef ENABLE_STATS
#define ENABLE_STATS 0
#endif

/*
Author: Amogh Lonkar, Scott Beamer, <Clover Authors>

Clover (Cover + optional IS on complement)

Counts occurrences of cliques of size k using Cover-based pivoting.
When ENABLE_IS=1, dense residual subgraphs are counted via
complement-graph independent sets (Hoede-Li identity).
*/

#if ENABLE_STATS
namespace {

constexpr int kNumTimeBins = 12;
constexpr int kNumDensityBins = 20;
constexpr std::array<double, kNumTimeBins + 1> kTimeEdges = {
    0.0, 1e-6, 1e-5, 1e-4, 1e-3, 1e-2, 1e-1,
    1.0, 10.0, 100.0, 1000.0, 10000.0, 100000.0};

struct RootStats {
  uint64_t tree_size = 0;
  int max_depth = 0;
};

struct StatsAccumulator {
  uint64_t tree_size = 0;
  int max_depth = 0;
  double max_root_s = 0.0;
  double total_root_s = 0.0;
  std::array<uint64_t, kNumTimeBins> time_counts{};
  std::array<double, kNumTimeBins> time_sums{};
  std::array<uint64_t, kNumDensityBins> density_counts{};
  std::array<double, kNumDensityBins> density_time_sums{};

  void add_root(double elapsed_s, double density, const RootStats& root) {
    tree_size += root.tree_size;
    max_depth = std::max(max_depth, root.max_depth);
    max_root_s = std::max(max_root_s, elapsed_s);
    total_root_s += elapsed_s;

    int tb = kNumTimeBins - 1;
    for (int i = 0; i < kNumTimeBins; i++) {
      if (elapsed_s < kTimeEdges[i + 1]) {
        tb = i;
        break;
      }
    }
    time_counts[tb]++;
    time_sums[tb] += elapsed_s;

    int db = static_cast<int>(density * kNumDensityBins);
    db = std::max(0, std::min(kNumDensityBins - 1, db));
    density_counts[db]++;
    density_time_sums[db] += elapsed_s;
  }

  void merge(const StatsAccumulator& other) {
    tree_size += other.tree_size;
    max_depth = std::max(max_depth, other.max_depth);
    max_root_s = std::max(max_root_s, other.max_root_s);
    total_root_s += other.total_root_s;
    for (int i = 0; i < kNumTimeBins; i++) {
      time_counts[i] += other.time_counts[i];
      time_sums[i] += other.time_sums[i];
    }
    for (int i = 0; i < kNumDensityBins; i++) {
      density_counts[i] += other.density_counts[i];
      density_time_sums[i] += other.density_time_sums[i];
    }
  }
};

static inline void stats_visit(RootStats* stats, int depth) {
  if (!stats) return;
  stats->tree_size++;
  stats->max_depth = std::max(stats->max_depth, depth);
}

static inline double root_density(sgNodeT nverts, size_t directed_edges) {
  if (nverts <= 1) return 0.0;
  return static_cast<double>(directed_edges) /
         static_cast<double>(nverts) /
         static_cast<double>(nverts - 1);
}

static void print_stats(int k, const StatsAccumulator& stats,
                        double count_time_s, int threads) {
  double efficiency = 0.0;
  if (threads > 0 && count_time_s > 0.0) {
    efficiency = stats.total_root_s / (static_cast<double>(threads) * count_time_s);
  }

  std::cout << "STAT_SUMMARY"
            << " k=" << k
            << " tree_size=" << stats.tree_size
            << " max_depth=" << stats.max_depth
            << " max_vertex_s=" << stats.max_root_s
            << " total_work_s=" << stats.total_root_s
            << " parallel_efficiency=" << efficiency
            << std::endl;

  for (int i = 0; i < kNumTimeBins; i++) {
    std::cout << "STAT_TIME_BIN"
              << " k=" << k
              << " bin_low=" << kTimeEdges[i]
              << " bin_high=" << kTimeEdges[i + 1]
              << " count=" << stats.time_counts[i]
              << " time_sum_s=" << stats.time_sums[i]
              << std::endl;
  }

  for (int i = 0; i < kNumDensityBins; i++) {
    const double low = static_cast<double>(i) / kNumDensityBins;
    const double high = static_cast<double>(i + 1) / kNumDensityBins;
    const uint64_t count = stats.density_counts[i];
    const double sum = stats.density_time_sums[i];
    const double avg = count == 0 ? 0.0 : sum / static_cast<double>(count);
    std::cout << "STAT_DENSITY_BIN"
              << " k=" << k
              << " density_low=" << low
              << " density_high=" << high
              << " avg_time=" << avg
              << " vertex_count=" << count
              << " time_sum_s=" << sum
              << std::endl;
  }
}

}  // namespace

#define STATS_RECURSE_ARG , RootStats* stats
#define STATS_RECURSE_PASS , stats
#define STATS_RECURSE_NULL , nullptr
#else
#define STATS_RECURSE_ARG
#define STATS_RECURSE_PASS
#define STATS_RECURSE_NULL
#endif


count_t PivotRecurse(SubGraph *sg, sgNodeT max_k, sgNodeT clique_size,
                     sgNodeT num_pivots, int depth STATS_RECURSE_ARG) {
#if ENABLE_STATS
  stats_visit(stats, depth);
#endif
  if ((sg->nverts_ + clique_size) < max_k)
    return 0;
  sgNodeT num_holds = clique_size - num_pivots;

  if (sg->nverts_ == 0 || (num_holds == max_k)) {
    return n_choose_k(num_pivots, max_k - num_holds);
  }

  if (num_holds == max_k - 1) {
    return sg->nverts_ + num_pivots;
  }
  if (num_holds == max_k - 2) {
    return n_choose_k(num_pivots, 2) + sg->nedges_/2 + (sg->nverts_ * num_pivots);
  }

#if ENABLE_IS
  if (complement::should_use(sg, max_k, clique_size, num_pivots)) {
    return complement::count_cliques(sg, max_k, clique_size, num_pivots);
  }
#endif

  sgNodeT pivot_id_r = sg->pivot_;
  count_t count = 0;
  sgNodeT num_unreachable = sg->CountUnreachableFromPivot(pivot_id_r);
  if (sg->pivot_deg_ == sg->nverts_ - 1) {
    sg->InduceFromSelfMutate(pivot_id_r);
    count += PivotRecurse(sg, max_k, clique_size+1, num_pivots+1, depth+1 STATS_RECURSE_PASS);
    sg->UndoSelfMutate();
  } else {
    sg->InduceFromSelfMutate(pivot_id_r);
    count += PivotRecurse(sg, max_k, clique_size+1, num_pivots+1, depth+1 STATS_RECURSE_PASS);
    sg->UndoSelfMutate();

    sgNodeT subproblem_k = max_k - clique_size;
    bool cover_success = false;
    bool cover_cond = ((num_unreachable > subproblem_k)
                    && (depth < max_k) && (num_holds < max_k-3)
                    && (clique_size <= max_k - 3));
    if (cover_cond) {
      cover_success = sg->firstBFT(pivot_id_r, subproblem_k);
    }

    if (cover_cond && cover_success) {
      auto iterbft_lvls = sg->iterBFT(pivot_id_r, subproblem_k);

      for (sgPackedT lvl_entry : iterbft_lvls) {
        sgNodeT e1 = unpack_a(lvl_entry);
        sg->expandLvl(pivot_id_r, subproblem_k);
        auto covernbr_to_induce = sg->findCoverNbr(e1);
        for (sgNodeT e2 : covernbr_to_induce) {
          sgNodeT lo = (e1 < e2) ? e1 : e2;
          sgNodeT hi = (e1 < e2) ? e2 : e1;
          assert(lo != hi);
          sgPackedT e = pack2ints(lo, hi);
          sg->InduceFromSelfMutate(e, pivot_id_r, subproblem_k);
          count += PivotRecurse(sg, max_k, clique_size+2, num_pivots, depth+1 STATS_RECURSE_PASS);
          sg->UndoSelfMutate();
        }
        sg->PopCoverNbr();
      }
      sg->PopCoverLevels();
    } else {
      auto verts_to_induce = sg->ActiveUnreachableFromPivot(pivot_id_r);
      for (sgNodeT v_r : verts_to_induce) {
        if (__builtin_expect((v_r != pivot_id_r), 1)) {
          sg->InduceFromSelfMutate(v_r, verts_to_induce);
          count += PivotRecurse(sg, max_k, clique_size+1, num_pivots, depth+1 STATS_RECURSE_PASS);
          sg->UndoSelfMutate();
        }
      }
      sg->PopNonNeighbors();
    }
  }
  return count;
}


count_t PivotCount(const Graph &dag, NodeID k) {
  count_t count = 0;
  #pragma omp parallel
  {
    SubGraph sg;
    #pragma omp for reduction(+:count) schedule(dynamic,1)
    for (NodeID v = 0; v < dag.num_nodes(); v++) {
      sg.InduceFromDAG(dag, v, k);
      count += PivotRecurse(&sg, k, 1, 0, 0 STATS_RECURSE_NULL);
    }
  }
  return count;
}


int main(int argc, char* argv[]) {
  CLKClique cli(argc, argv, "Clover clique counting", 3, false);
  if (!cli.ParseArgs()) {
    return -1;
  }
  Builder b(cli);
  Timer t;
  Graph dag;
  {
    Graph g = b.MakeGraph();
    if (g.directed()) {
      std::cout << "Input graph is directed but clique counting requires";
      std::cout << " undirected" << std::endl;
      std::exit(-2);
    }
    t.Start();
    dag = Ordering::Directionalize(g, b);
    t.Stop();
  }

  double direct_time = t.Seconds();
  dag.PrintStats();
  PrintStep("Max Degree", static_cast<int64_t>(Ordering::FindMaxDegree(dag)));
  PrintTime("Directing Time", direct_time);

  int k_min = cli.clique_start();
  int k_max = cli.clique_end();

  for (int k = k_min; k <= k_max; k++) {
    t.Start();
    count_t k_count = 0;
#if ENABLE_STATS
    StatsAccumulator stats_total;
#endif

    #pragma omp parallel
    {
      SubGraph sg;
      count_t local_count = 0;
#if ENABLE_STATS
      StatsAccumulator local_stats;
#endif
      #pragma omp for schedule(dynamic,1)
      for (NodeID v = 0; v < dag.num_nodes(); v++) {
        sg.InduceFromDAG(dag, v, k);
#if ENABLE_STATS
        RootStats root_stats;
        const double root_start = omp_get_wtime();
        local_count += PivotRecurse(&sg, k, 1, 0, 0, &root_stats);
        const double elapsed = omp_get_wtime() - root_start;
        local_stats.add_root(elapsed, root_density(sg.nverts_, sg.nedges_), root_stats);
#else
        local_count += PivotRecurse(&sg, k, 1, 0, 0);
#endif
      }

      #pragma omp atomic
      k_count += local_count;

#if ENABLE_STATS
      #pragma omp critical
      stats_total.merge(local_stats);
#endif
    }

    t.Stop();
    double count_time = t.Seconds();

    PrintTime("Counting Time", count_time);
    PrintTime("Total Time", direct_time + count_time);
    std::cout << "k: ";
    PrintCliqueCountRow(k, k_count);
#if ENABLE_STATS
    print_stats(k, stats_total, count_time, omp_get_max_threads());
#endif
  }
  return 0;
}
