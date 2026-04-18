// Copyright (c) 2025, The Regents of the University of California (Regents)
// See LICENSE for license details

#include "pivotscale.h"
#if ENABLE_IS
#include "complement_count.h"
#endif

/*
Clover (Cover + optional IS on complement)

Counts occurrences of cliques of size k using Cover-based pivoting.
When ENABLE_IS=1, dense residual subgraphs are counted via
complement-graph independent sets (Hoede-Li identity).
*/


count_t PivotRecurse(SubGraph *sg, sgNodeT max_k, sgNodeT clique_size,
                     sgNodeT num_pivots, int depth) {
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
    count += PivotRecurse(sg, max_k, clique_size+1, num_pivots+1, depth+1);
    sg->UndoSelfMutate();
  } else {
    sg->InduceFromSelfMutate(pivot_id_r);
    count += PivotRecurse(sg, max_k, clique_size+1, num_pivots+1, depth+1);
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
          count += PivotRecurse(sg, max_k, clique_size+2, num_pivots, depth+1);
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
          count += PivotRecurse(sg, max_k, clique_size+1, num_pivots, depth+1);
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
      count += PivotRecurse(&sg, k, 1, 0, 0);
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

    #pragma omp parallel
    {
      SubGraph sg;
      count_t local_count = 0;
      #pragma omp for schedule(dynamic,1)
      for (NodeID v = 0; v < dag.num_nodes(); v++) {
        sg.InduceFromDAG(dag, v, k);
        local_count += PivotRecurse(&sg, k, 1, 0, 0);
      }

      #pragma omp atomic
      k_count += local_count;
    }

    t.Stop();
    double count_time = t.Seconds();

    PrintTime("Counting Time", count_time);
    PrintTime("Total Time", direct_time + count_time);
    std::cout << "k: ";
    PrintCliqueCountRow(k, k_count);
  }
  return 0;
}
