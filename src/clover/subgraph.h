// Copyright (c) 2025, The Regents of the University of California (Regents)
// See LICENSE for license details

#ifndef SUBGRAPH_BIT_H_
#define SUBGRAPH_BIT_H_

#include <algorithm>
#include <iostream>
#include <span>
#include <utility>
#include <vector>
#include <set>
#include <queue>

#include "benchmark.h"
#include "graph.h"
#include "grouped_stack.h"
#include "packbit.h"


#define TAIL 1
#define EDGETAIL 1
#ifndef EFFCOV
#define EFFCOV 1
#endif

#define EDGESTACK 0

typedef int16_t sgNodeT;
typedef int32_t sgPackedT;

// typedef int64_t EdgeT;
// typedef int16_t LvlT;

/****** BitSet: compact boolean array with efficient set-bit iteration ******
 *
 * Replaces both active_ (dense boolean lookup) and active_list_ (active-vertex
 * enumeration).  Iteration via range(n) scans 64 bits at a time using ctzll,
 * visiting only words that contain at least one set bit.
 *
 * Usage:
 *   for (sgNodeT v : activebit_.range(nverts_cap_)) { ... }
 *
 * Clearing a bit during iteration is safe: the iterator works on a local copy
 * of each 64-bit word, so a clear() on an already-visited position has no
 * effect on the walk.
 ***************************************************************************/
struct BitSet {
  std::vector<uint64_t> words_;

  void resize(sgNodeT n) {
    sgNodeT nw = (n + 63) >> 6;
    if (nw > (sgNodeT)words_.size())
      words_.resize(nw, 0ULL);
  }

  /* clear bits [0, n) — grow-only vector, so only touch the used prefix */
  void fill_false(sgNodeT n) {
    sgNodeT nw = (n + 63) >> 6;
    std::fill(words_.begin(), words_.begin() + nw, 0ULL);
  }

  void set  (sgNodeT i) { words_[i >> 6] |=  (1ULL << (i & 63)); }
  void clear(sgNodeT i) { words_[i >> 6] &= ~(1ULL << (i & 63)); }
  bool test (sgNodeT i) const { return (words_[i >> 6] >> (i & 63)) & 1; }

  /* ---- iterator over positions of set bits ---- */
  struct Iter {
    const uint64_t* words;
    sgNodeT nwords;
    sgNodeT widx;
    uint64_t cur;   /* local copy of current word */

    sgNodeT operator*() const { return (widx << 6) + __builtin_ctzll(cur); }

    Iter& operator++() {
      cur &= cur - 1;                       /* clear lowest set bit */
      while (cur == 0 && ++widx < nwords)
        cur = words[widx];
      return *this;
    }

    bool operator!=(const Iter& o) const {
      return widx != o.widx || cur != o.cur;
    }
  };

  struct Range {
    const uint64_t* words;
    sgNodeT nwords;

    Iter begin() const {
      for (sgNodeT i = 0; i < nwords; i++)
        if (words[i]) return {words, nwords, i, words[i]};
      return end();
    }
    Iter end() const { return {words, nwords, nwords, 0}; }
  };

  /* range-for over set-bit positions in [0, n) */
  Range range(sgNodeT n) const {
    return {words_.data(), static_cast<sgNodeT>((n + 63) >> 6)};
  }
};

/* TODO:
- overload active_scratch_ with bfs_levels instead.
- combine dropped_verts & dropped_verts_tail 
- clean up setting active_bit in induceFromDAG
- mask deg < max_k vertices from induceFromDAG and pivotcount openmp fork
*/

class SubGraph{
  /* graph
   *   activebit_      : bit i set  <=>  vertex i is currently active.
   *                     Replaces active_ (dense boolean) + active_list_.
   *   active_scratch_ : uint8_t array used ONLY inside InduceFromSelfMutate
   *                     to mark the new-active state (and as a counter in
   *                     overload 3).  Zero outside of those calls.           */
  BitSet activebit_;
  std::vector<uint8_t> active_scratch_;
  std::vector<std::vector<sgNodeT>> adj_list_;
  sgNodeT nverts_cap_ = 0;   /* vertex-ID range [0, nverts_cap_) for this subgraph;
                                set at InduceFromDAG, unchanged during mutation  */

  /* backtracking stack */
  GroupedStack<sgNodeT> dropped_verts_;
  #if TAIL
    GroupedStack<sgNodeT> dropped_verts_tails_;
  #endif
  #if EDGETAIL
      GroupedStack<sgPackedT> active_verts_tails_;
  #endif


  /* bfs */
  std::vector<sgNodeT> bfs_levels_;
  std::vector<sgNodeT> prev_levels_;
  sgNodeT curr_lvl;
  std::queue<sgNodeT> q;

  /* scratch */
  std::vector<NodeID> origID_;
  sgNodeT adj_list_max_nverts_ = 0;

 public:
  std::vector<sgNodeT> active_tails_;
  sgNodeT nverts_ = 0;
  size_t  nedges_ = 0;
  sgNodeT pivot_     = -1;
  sgNodeT pivot_deg_ = -1;
  int    last_bft_totallevels_ = 0;
  std::vector<sgNodeT> levelcount;

  /* recursion stack */
  GroupedStack<sgNodeT> pivot_non_neighs_;
  #if EFFCOV
    GroupedStack<sgPackedT> cover_levels_;

    GroupedStack<sgNodeT> cover_nbr_;
    #if EDGESTACK
      GroupedStack<sgPackedT> cover_edges_;
    #endif 
  #endif

  // Iterate over active vertex IDs (no copy — returns a range over the bitset)
  auto active_range() const { return activebit_.range(nverts_cap_); }
  NodeID vertex_cap() const { return nverts_cap_; }

  SubGraph() {}

  void InduceFromDAG(const Graph &dag, NodeID u, sgNodeT max_k) {
    nverts_     = dag.out_degree(u);
    nverts_cap_ = nverts_;
    if (nverts_ > (sgNodeT)active_scratch_.size()) {
      active_scratch_.resize(nverts_, 0);
      active_tails_.resize(nverts_);
      bfs_levels_.resize(nverts_, -2);
      prev_levels_.resize(nverts_, -2);
    }
    activebit_.resize(nverts_);
    activebit_.fill_false(nverts_);
    if (nverts_ > adj_list_max_nverts_) {
      adj_list_.resize(nverts_);
      adj_list_max_nverts_ = nverts_;
    }

    dropped_verts_.clear();
    dropped_verts_.reserve(nverts_);
    #if TAIL
      dropped_verts_tails_.clear();
      dropped_verts_tails_.reserve(nverts_);
    #endif
    pivot_non_neighs_.clear();
    pivot_non_neighs_.reserve(nverts_);

    std::fill(bfs_levels_.begin(),  bfs_levels_.begin()  + nverts_, -2);
    std::fill(prev_levels_.begin(), prev_levels_.begin() + nverts_, -2);

    origID_.clear();
    sgNodeT v_r = 0;
    for (NodeID v_g : dag.out_neigh(u)) {
      origID_.push_back(v_g);
      activebit_.set(v_r);
      adj_list_[v_r].clear();
      adj_list_[v_r].reserve(dag.out_degree(v_g) * 2); 
      v_r++;
    }

    for (sgNodeT v_i = 0; v_i < nverts_; v_i++) {
      auto   neigh = dag.out_neigh(origID_[v_i]);
      NodeID v_deg = (NodeID)dag.out_degree(origID_[v_i]);
      sgNodeT v_i_tail = adj_list_[v_i].size();
      sorted_intersection(&origID_[0], nverts_,
                          neigh.begin(), v_deg,
                          std::back_inserter(adj_list_[v_i]));
      for (sgNodeT j = v_i_tail; j < (sgNodeT)adj_list_[v_i].size(); j++)
        adj_list_[adj_list_[v_i][j]].push_back(v_i);
    }

    for (auto& adj : adj_list_) {
      adj.shrink_to_fit();
    }

    nedges_ = 0; pivot_ = -1; pivot_deg_ = -1;
    for (sgNodeT v_r : activebit_.range(nverts_cap_)) {
      sgNodeT d = adj_list_[v_r].size();
      active_tails_[v_r] = d;
      nedges_ += d;
      if (d > pivot_deg_) { pivot_deg_ = d; pivot_ = v_r; }
    }
    levelcount.reserve(nverts_);

    #if EFFCOV
      cover_nbr_.clear();
      cover_nbr_.reserve(nverts_);
      #if EDGESTACK
        cover_edges_.clear();
        cover_edges_.reserve(nverts_ * nedges_);
      #endif
        cover_levels_.clear();
      cover_levels_.reserve(nverts_);
    #endif
    #if EDGETAIL
      active_verts_tails_.clear();
      active_verts_tails_.reserve(nedges_);
    #endif
    assert(nverts_ <= std::numeric_limits<sgNodeT>::max());
  }

  void InduceFromDAG(const Graph &dag, NodeID u1, NodeID u2, sgNodeT max_k) {
    origID_.clear();
    auto n1 = dag.out_neigh(u1);
    auto n2 = dag.out_neigh(u2);
    std::set_intersection(n1.begin(), n1.end(), n2.begin(), n2.end(),
                          std::back_inserter(origID_));

    nverts_     = origID_.size();
    nverts_cap_ = nverts_;
    if (nverts_ > (sgNodeT)active_scratch_.size()) {
      active_scratch_.resize(nverts_, 0);
      active_tails_.resize(nverts_);
      bfs_levels_.resize(nverts_, -2);
      prev_levels_.resize(nverts_, -2);
    }
    activebit_.resize(nverts_);
    activebit_.fill_false(nverts_);
    if (nverts_ > adj_list_max_nverts_) {
      adj_list_.resize(nverts_);
      adj_list_max_nverts_ = nverts_;
    }

    dropped_verts_.clear();
    dropped_verts_.reserve(nverts_);
    #if TAIL
      dropped_verts_tails_.clear();
      dropped_verts_tails_.reserve(nverts_);
    #endif
    pivot_non_neighs_.clear();
    pivot_non_neighs_.reserve(nverts_);

    std::fill(bfs_levels_.begin(),  bfs_levels_.begin()  + nverts_, -2);
    std::fill(prev_levels_.begin(), prev_levels_.begin() + nverts_, -2);

    sgNodeT v_r = 0;
    for (NodeID v_g : origID_) {
      activebit_.set(v_r);
      adj_list_[v_r].clear();
      adj_list_[v_r].reserve(dag.out_degree(v_g) * 2); 
      v_r++;
    }

    for (sgNodeT v_i = 0; v_i < nverts_; v_i++) {
      auto   neigh = dag.out_neigh(origID_[v_i]);
      NodeID v_deg = (NodeID)dag.out_degree(origID_[v_i]);
      sgNodeT v_i_tail = adj_list_[v_i].size();
      sorted_intersection(&origID_[0], nverts_,
                          neigh.begin(), v_deg,
                          std::back_inserter(adj_list_[v_i]));
      for (sgNodeT j = v_i_tail; j < (sgNodeT)adj_list_[v_i].size(); j++)
        adj_list_[adj_list_[v_i][j]].push_back(v_i);
    }

    for (auto& adj : adj_list_) {
      adj.shrink_to_fit();
    }

    nedges_ = 0; pivot_ = -1; pivot_deg_ = -1;
    for (sgNodeT v_r : activebit_.range(nverts_cap_)) {
      sgNodeT d = adj_list_[v_r].size();
      active_tails_[v_r] = d;
      nedges_ += d;
      if (d > pivot_deg_) { pivot_deg_ = d; pivot_ = v_r; }
    }
    levelcount.reserve(nverts_);

    #if EFFCOV
      cover_nbr_.clear();
      cover_nbr_.reserve(nverts_);
      #if EDGESTACK
        cover_edges_.clear();
        cover_edges_.reserve(nverts_ * nedges_);
      #endif
        cover_levels_.clear();
      cover_levels_.reserve(nverts_);
    #endif
    #if EDGETAIL
      active_verts_tails_.clear();
      active_verts_tails_.reserve(nedges_);
    #endif

    assert(nverts_ <= std::numeric_limits<sgNodeT>::max());
  }

  template<typename OutItr_>
  void sorted_intersection(const NodeID* v1ptr, NodeID v1size,
                           const NodeID* v2ptr, sgNodeT v2size,
                           OutItr_ out) {
    NodeID v1idx = 0;
    sgNodeT v2idx = 0;
    while (v1idx < v1size && v2idx < v2size) {
      if      (v1ptr[v1idx] < v2ptr[v2idx]) { ++v1idx; }
      else if (v2ptr[v2idx] < v1ptr[v1idx]) { ++v2idx; }
      else { *out++ = v1idx; ++v1idx; ++v2idx; }
    }
  }

  /******************* INDUCE FROM SELF MUTATE *********************/
  /* active_scratch_ protocol:
   *   - Before call:  all zeros (invariant maintained by cleanup at end).
   *   - During call:  used as new-active marker (1 = keep, 0 = drop).
   *                   Overload 3 also uses values 0/1/2 as a counter.
   *   - After call:   restored to all zeros.
   * activebit_ is updated in-place: bits for dropped vertices are cleared.
   * Clearing a bit during activebit_.range() iteration is safe because the
   * iterator holds a local copy of each 64-bit word.                        */

  /* Overload 1: Induce from pivot */
  void InduceFromSelfMutate(sgNodeT p_r) {
    for (sgNodeT v_r : Neighs(p_r))
      active_scratch_[v_r] = 1;

    dropped_verts_.create_new_frame();
    #if TAIL
      dropped_verts_tails_.create_new_frame();
    #endif
    #if EDGETAIL
      active_verts_tails_.create_new_frame();
    #endif
    nedges_ = 0; pivot_ = -1; pivot_deg_ = -1;

    for (sgNodeT n_r : activebit_.range(nverts_cap_)) {
      if (active_scratch_[n_r]) {
        MoveNbrToTail(n_r, [this](sgNodeT, sgNodeT v4_r){ return !active_scratch_[v4_r]; });
      } else {
        activebit_.clear(n_r);
        dropped_verts_.push_back(n_r);
        #if TAIL
          dropped_verts_tails_.push_back(active_tails_[n_r]);
        #endif
        nverts_--;
      }
    }

    /* restore active_scratch_ to all-zero */
    for (sgNodeT v_r : Neighs(p_r))
      active_scratch_[v_r] = 0;
  }

  /* Overload 2: Induce from non-neighbor vertex */
  void InduceFromSelfMutate(sgNodeT u_r, const std::span<const sgNodeT> &excl_verts) {
    for (sgNodeT v_r : Neighs(u_r))
      active_scratch_[v_r] = 1;
    for (sgNodeT n_r : excl_verts)
      if (n_r < u_r) active_scratch_[n_r] = 0;

    dropped_verts_.create_new_frame();
    #if TAIL
      dropped_verts_tails_.create_new_frame();
    #endif
    #if EDGETAIL
      active_verts_tails_.create_new_frame();
    #endif
    nedges_ = 0; pivot_ = -1; pivot_deg_ = -1;

    for (sgNodeT n_r : activebit_.range(nverts_cap_)) {
      if (active_scratch_[n_r]) {
        MoveNbrToTail(n_r, [this](sgNodeT, sgNodeT v4_r){ return !active_scratch_[v4_r]; });
      } else {
        activebit_.clear(n_r);
        dropped_verts_.push_back(n_r);
        #if TAIL
          dropped_verts_tails_.push_back(active_tails_[n_r]);
        #endif
        nverts_--;
      }
    }

    /* restore active_scratch_ to all-zero (only Neighs(u_r) were set) */
    for (sgNodeT v_r : Neighs(u_r))
      active_scratch_[v_r] = 0;
  }

  /* Overload 3: Induce from cover edge */
  // void InduceFromSelfMutate(EdgeT e, sgNodeT pivot_r, const std::span<const EdgeT> &excl, int k) {
  void InduceFromSelfMutate(sgPackedT e, sgNodeT pivot_r, int k) {
    #if EFFCOV
    expandLvl(pivot_r, k);
    #endif
    sgNodeT v1_r = unpack_a(e);
    sgNodeT v2_r = unpack_b(e);

    /* counting phase: active_scratch_ acts as a 0/1/2 counter */
    for (sgNodeT n_r : activebit_.range(nverts_cap_)) active_scratch_[n_r] = 0;
    for (sgNodeT w_r : Neighs(v1_r)) active_scratch_[w_r]++;
    for (sgNodeT w_r : Neighs(v2_r)) active_scratch_[w_r]++;

    for (sgNodeT n_r : activebit_.range(nverts_cap_)) {
      if (active_scratch_[n_r] <= 1) {
        active_scratch_[n_r] = 0;
        continue;
      }
      #if EFFCOV
        sgNodeT minv1 = n_r < v1_r ? n_r : v1_r;
        sgNodeT maxv1 = n_r < v1_r ? v1_r : n_r;
        if (isCoverEdges(minv1, maxv1) && packed_compare(minv1, maxv1, e))
          active_scratch_[n_r] = 0;

        sgNodeT minv2 = n_r < v2_r ? n_r : v2_r;
        sgNodeT maxv2 = n_r < v2_r ? v2_r : n_r;
        if (isCoverEdges(minv2, maxv2) && packed_compare(minv2, maxv2, e))
          active_scratch_[n_r] = 0;
      #endif
    }

    dropped_verts_.create_new_frame();
    #if TAIL
      dropped_verts_tails_.create_new_frame();
    #endif
    #if EDGETAIL
      active_verts_tails_.create_new_frame();
    #endif
    nedges_ = 0; pivot_ = -1; pivot_deg_ = -1;

    for (sgNodeT n_r : activebit_.range(nverts_cap_)) {
      if (active_scratch_[n_r]) {
        #if EFFCOV
          if (n_r < v1_r) {
            MoveNbrToTail(n_r, [this](sgNodeT v3_r, sgNodeT v4_r) {
              if (!active_scratch_[v4_r]) return true;
              sgNodeT lo = v3_r < v4_r ? v3_r : v4_r;
              sgNodeT hi = v3_r < v4_r ? v4_r : v3_r;
              sgNodeT lolvl = bfs_levels_[lo], hivl = bfs_levels_[hi];
              return (lolvl == 1 && hivl == 2) || (lolvl == 2 && hivl == 1) ||
                     (lolvl >= 2 && lolvl == hivl);
            });
          } else {
            MoveNbrToTail(n_r, [this, &v1_r](sgNodeT v3_r, sgNodeT v4_r) {
              if (!active_scratch_[v4_r]) return true;
              if (v4_r >= v1_r) return false;
              sgNodeT lo = v3_r < v4_r ? v3_r : v4_r;
              sgNodeT hi = v3_r < v4_r ? v4_r : v3_r;
              sgNodeT lolvl = bfs_levels_[lo], hivl = bfs_levels_[hi];
              return (lolvl == 1 && hivl == 2) || (lolvl == 2 && hivl == 1) ||
                     (lolvl >= 2 && lolvl == hivl);
            });
          }
        #endif
      } else {
        activebit_.clear(n_r);
        dropped_verts_.push_back(n_r);
        #if TAIL
          dropped_verts_tails_.push_back(active_tails_[n_r]);
        #endif
        nverts_--;
      }
    }

    /* restore active_scratch_ to all-zero (kept vertices are still set) */
    for (sgNodeT n_r : activebit_.range(nverts_cap_))
      active_scratch_[n_r] = 0;
  }

  /********************** INDUCEFROMSELFMUTATE HELPER *************************/
  template<typename Filter_>
  inline void MoveNbrToTail(sgNodeT n_r, Filter_ RemoveCondition) {
    sgNodeT* data = adj_list_[n_r].data();
    sgNodeT  tail = active_tails_[n_r];
    #if EDGETAIL
      sgNodeT original_tail = tail;
    #endif

    for (sgNodeT j = 0; j < tail; j++) {
      sgNodeT v_r = data[j];
      if (RemoveCondition(n_r, v_r)) {
        tail--;
        while (tail > j && RemoveCondition(n_r, data[tail]))
          tail--;
        if (tail > j)
          std::swap(data[j], data[tail]);
      }
    }
    active_tails_[n_r] = tail;

    #if EDGETAIL
      if (original_tail != tail)
        active_verts_tails_.push_back(pack2ints(n_r, original_tail));
      if (tail > pivot_deg_) { pivot_ = n_r; pivot_deg_ = tail; }
      nedges_ += tail;
    #endif
  }

  void UndoSelfMutate() {
    #if EDGETAIL
      for (sgPackedT vert_tail : active_verts_tails_.last_frame_iter()) {
        active_tails_[unpack_a(vert_tail)] = unpack_b(vert_tail);
      }
      active_verts_tails_.pop_frame();
    #endif

    auto inactive_verts = dropped_verts_.last_frame_iter();
    #if TAIL
      auto inactive_tails = dropped_verts_tails_.last_frame_iter();
    #endif
    sgNodeT sz = inactive_verts.size();
    for (sgNodeT i = 0; i < sz; i++) {
      sgNodeT n_r = inactive_verts[i];
      activebit_.set(n_r);
      nverts_++;
      #if TAIL
        active_tails_[n_r] = inactive_tails[i];
      #endif
    }
    dropped_verts_.pop_frame();
    #if TAIL
      dropped_verts_tails_.pop_frame();
    #endif
  }

  /************************* BFS & COVER EDGES FUNCTIONS ************/
  // template<typename Cond_>
  // void BFS(NodeID source, Cond_ unvisited) {
  //   bfs_levels_[source] = curr_lvl;
  //   curr_lvl_count = 1;
  //   first_node_in_lvl = source;

  //   LvlT prev;
  //   q.push(source);
  //   while (!q.empty()) {
  //     NodeID u = q.front(); q.pop();
  //     for (NodeID v : Neighs(u)) {
  //       if (unvisited(u, v)) {
  //         prev = bfs_levels_[v];
  //         bfs_levels_[v] = bfs_levels_[u] + 1;
  //         if (bfs_levels_[v] > curr_lvl) {
  //           if(curr_lvl >= 3){
  //             if (curr_lvl_count == 1)
  //               singleinlvl.push_back(first_node_in_lvl);
  //             curr_lvl_count = 1;
  //             first_node_in_lvl = v;
  //           }
  //           curr_lvl = bfs_levels_[v];
  //         } else {
  //           if (prev != bfs_levels_[v] && curr_lvl >= 3) curr_lvl_count++;
  //         }
  //         q.push(v);
  //       }
  //     }
  //   }
  //   curr_lvl++;
  //   if (curr_lvl_count == 1 && curr_lvl >= 3 )
  //     singleinlvl.push_back(first_node_in_lvl);
  //   for (const NodeID s_r : singleinlvl) bfs_levels_[s_r] = -2;
  //   singleinlvl.clear();
  // }

  template<typename Cond_>
  void BFS(sgNodeT source, Cond_ unvisited) {
    while (!q.empty()) q.pop();

    bfs_levels_[source] = curr_lvl;
    levelcount.push_back(1);
    assert(levelcount.size() == curr_lvl + 1);
    q.push(source);

    while (!q.empty()) {
      sgNodeT u = q.front(); q.pop();
      for (sgNodeT v : Neighs(u)) {
        if (unvisited(u, v)) {
          sgNodeT vlvl = bfs_levels_[u] + 1;
          bfs_levels_[v] = vlvl;
          if (bfs_levels_[v] > curr_lvl){
            curr_lvl = bfs_levels_[v];
            levelcount.push_back(0);
          }
          levelcount[vlvl]++;
          q.push(v);
        }
      }
    }
    curr_lvl++;
  }

  bool BFT(sgNodeT pivot_id_r, int k) {
    levelcount.clear();
    std::fill(bfs_levels_.begin(), bfs_levels_.end(), -2);
    for (sgNodeT i : activebit_.range(nverts_cap_))
      if (active_tails_[i] >= k - 1) bfs_levels_[i] = -1;

    curr_lvl = 0;
    BFS(pivot_id_r, [this](sgNodeT, sgNodeT v) { return bfs_levels_[v] == -1; });
    if(curr_lvl == 1){//to avoid next source from being masked 
      curr_lvl = 2;
      levelcount.push_back(0);
    }
    for (sgNodeT n_r : activebit_.range(nverts_cap_)){
      if (bfs_levels_[n_r] == -1){
        BFS(n_r, [this](sgNodeT, sgNodeT v) { return bfs_levels_[v] == -1; });
      }
    }
    bfs_levels_[pivot_id_r] = 0;
    return curr_lvl;
  }

  int32_t BFTMasked(int k) {
    levelcount.clear();
    std::fill(bfs_levels_.begin(), bfs_levels_.end(), -2);
    for (sgNodeT i : activebit_.range(nverts_cap_))
      if (prev_levels_[i] != -2) bfs_levels_[i] = -1;

    levelcount.resize(3,0);
    curr_lvl = 3;
    for (sgNodeT n_r : activebit_.range(nverts_cap_)) {
      if (bfs_levels_[n_r] == -1)
        BFS(n_r, [this](sgNodeT u, sgNodeT v) {
          return bfs_levels_[v] == -1 && prev_levels_[v] == prev_levels_[u];
        });
    }
    for (sgNodeT u_r : activebit_.range(nverts_cap_)) {
      sgNodeT lvl = bfs_levels_[u_r];
      if(lvl >= 3 && levelcount[lvl] < k){
        bfs_levels_[u_r] = -2;
      }
    }
    return curr_lvl;
  }

  inline bool firstBFT(sgNodeT pivot_id_r, int k) {
    BFT(pivot_id_r, k);
    return (curr_lvl <=2);
  }

  std::span<const sgPackedT> iterBFT(sgNodeT pivot_id_r, int k) {  
    // BFT(pivot_id_r, k);
    // return (curr_lvl > 2); /* moved to firstBFT */

    #if EFFCOV
      StackCrossEdges(pivot_id_r, k);
    #endif
    k = (k + 1) / 2;
    // int iter = 0;
    while (k > 2) {
      prev_levels_ = bfs_levels_;
      BFTMasked(k);
      k = (k + 1) / 2;
      // iter++;
    }
    #if EFFCOV
      return StackLevelEdges();
    #endif
  }

#if EFFCOV
  void StackCrossEdges(sgNodeT p_r, int k) {
    #if EDGESTACK
      cover_edges_.create_new_frame();
      for (sgNodeT u_r : activebit_.range(nverts_cap_)) {
        sgNodeT lvl = bfs_levels_[u_r];
        if (lvl < 0 || lvl > 2) continue;
        for (sgNodeT v_r : Neighs(u_r)) {
          if (u_r < v_r) {
            sgNodeT vlvl = bfs_levels_[v_r];
            if ((lvl == 1 && vlvl == 2) || (lvl == 2 && vlvl == 1) ||
                (lvl >= 2 && lvl == vlvl))
              cover_edges_.push_back(pack2ints(u_r, v_r));
          }
        }
      }
    #endif

    cover_levels_.create_new_frame();
    cover_levels_.push_back(pack2ints(p_r, static_cast<sgNodeT>(0)));
    for (sgNodeT u_r : activebit_.range(nverts_cap_)) {
      sgNodeT lvl = bfs_levels_[u_r];
      if (lvl == 2) cover_levels_.push_back(pack2ints(u_r, static_cast<sgNodeT>(lvl)));
      if ((lvl >= 1 && lvl <= 2) || (lvl >= 0 && levelcount[lvl] < k)) {          
        bfs_levels_[u_r] = -2;
      }
    }
  }

  inline std::span<const sgPackedT> StackLevelEdges() {
    for (sgNodeT u_r : activebit_.range(nverts_cap_)) {
      sgNodeT lvl = bfs_levels_[u_r];
      if (lvl > 2) {
        cover_levels_.push_back(pack2ints(u_r, static_cast<sgNodeT>(lvl)));
        #if EDGESTACK
          for (sgNodeT v_r : Neighs(u_r))
            if ((u_r < v_r) && lvl == bfs_levels_[v_r])
              cover_edges_.push_back(pack2ints(u_r, v_r));
        #endif 
      }
    }

    // return cover_edges_.last_frame_iter();
    return cover_levels_.last_frame_iter();
  }

  inline std::span<const sgNodeT> findCoverNbr(sgNodeT v_r){
    cover_nbr_.create_new_frame();
    /* vlvl from cover_levels_, can only be level 2 onwards */
    sgNodeT vlvl = bfs_levels_[v_r];
    if(vlvl == 2){
      for(sgNodeT n_r : Neighs(v_r)){
        if((bfs_levels_[n_r] == 1)|| (bfs_levels_[n_r] == vlvl))
          cover_nbr_.push_back(n_r);
      }
    }else{
      for(sgNodeT n_r : Neighs(v_r)){
        if(bfs_levels_[n_r] == vlvl)
          cover_nbr_.push_back(n_r);
      }
    }
    return cover_nbr_.last_frame_iter();
  } 

  inline void PopCoverNbr() { cover_nbr_.pop_frame(); }

  inline void expandLvl(sgNodeT pivot_r, int k){
    std::fill(bfs_levels_.begin(), bfs_levels_.end(), -2);
    bfs_levels_[pivot_r] = 0;
    for (const auto& l : cover_levels_.last_frame_iter()){
      bfs_levels_[unpack_a(l)] = unpack_b(l);
    }
    /* do not record level 0 & 1 to save memory - similar to pivotnonneigh*/
    for (sgNodeT v_r : Neighs(pivot_r)){
      if(active_tails_[v_r] >= k - 1){
        bfs_levels_[v_r] = 1;
      }
      // else{
      //   bfs_levels_[v_r] = -2;
      // }
    }
  }

  inline void PopCoverLevels() { cover_levels_.pop_frame(); }
  #if EDGESTACK
    inline void PopCoverEdges()  { cover_edges_.pop_frame(); }
  #endif 

  inline bool isCoverEdges(const sgNodeT e1, const sgNodeT e2) {
    sgNodeT e1lvl = bfs_levels_[e1];
    sgNodeT e2lvl = bfs_levels_[e2];
    /* real condition as follow, but always used on e1 < e2*/
    // return ((e1 < e2) && ((e1lvl == 1 && e2lvl == 2) ||
    //                       (e1lvl == 2 && e2lvl == 1) ||
    //                       (e1lvl >= 2 && e1lvl == e2lvl)));
    return ((e1lvl == 1 && e2lvl == 2) ||
            (e1lvl == 2 && e2lvl == 1) ||
            (e1lvl >= 2 && e1lvl == e2lvl));
  }
  
#endif

  // inline bool packed_compare(const EdgeT& e1, const EdgeT& e2) {
  //   NodeID e1_first  = unpack_a(e1);
  //   NodeID e1_second = unpack_b(e1);
  //   NodeID e2_first  = unpack_a(e2);
  //   NodeID e2_second = unpack_b(e2);
  //   if (e1_first != e2_first) return e1_first < e2_first;
  //   return e1_second < e2_second;
  // }

  /************************  UNCHANGED ***********************/

  std::span<const sgNodeT> ActiveUnreachableFromPivot(sgNodeT u_r) {
    pivot_non_neighs_.create_new_frame();
    /* mark neighbors in active_scratch_ */
    for (sgNodeT v_r : Neighs(u_r))
      active_scratch_[v_r] = 1;
    /* collect non-neighbors; restore scratch */
    for (sgNodeT n_r : activebit_.range(nverts_cap_)) {
      if (!active_scratch_[n_r])
        pivot_non_neighs_.push_back(n_r);
      else
        active_scratch_[n_r] = 0;
    }
    return pivot_non_neighs_.last_frame_iter();
  }

  inline sgNodeT CountUnreachableFromPivot(sgNodeT p_r) {
    return nverts_ - active_tails_[p_r];
  }

  inline std::span<const sgNodeT> Neighs(sgNodeT u_r) const {
    const sgNodeT* p = adj_list_[u_r].data();
    return std::span(p, p + active_tails_[u_r]);
  }

  inline void PopNonNeighbors() { pivot_non_neighs_.pop_frame(); }

  void PrintTopoStats() {
    sgNodeT num_nodes_active = 0;
    size_t  num_edges_active = 0;
    for (sgNodeT n_r : activebit_.range(nverts_cap_)) {
      num_nodes_active++;
      num_edges_active += active_tails_[n_r];
    }
    std::cout << "nodes: " << num_nodes_active;
    std::cout << " edges: " << num_edges_active << std::endl;
  }

  void PrintTopology() const {
    for (sgNodeT u_r : activebit_.range(nverts_cap_)) {
      std::cout << u_r << ": ";
      for (sgNodeT v_r : Neighs(u_r)) std::cout << v_r << " ";
      std::cout << std::endl;
    }
  }
};

#if ENABLE_IS
/****** CompGraph: complement graph with mutable adjacency lists ******
 *
 * Used by complement_count.h for independent set counting on dense subgraphs.
 *   - adj[v][0..tail[v])  = active complement neighbors
 *   - drop(v)             = swap v past tail in all neighbors' lists
 *   - undo_mutate()       = restore from GroupedStack
 *   - activebit[v]        = O(1) membership test
 *   - mark[v]             = scratch, within-level only
 *
 * Allocated once per thread. build() reuses grow-only buffers.
 * begin_mutate() / mutate_drop() / undo_mutate() for backtracking.
 *
 * Data:
 *   adj       : vector<vector<int>>  — complement neighbor lists (mutable order)
 *   tail      : vector<int>          — active tail per vertex: Neighs = adj[v][0..tail[v])
 *   activebit : vector<uint8_t>      — 1 if vertex is active
 *   mark      : vector<uint8_t>      — scratch, set/use/clear within single level
 *   dropped_verts / dropped_tails : GroupedStack — undo state (total ≤ nv)
 *   local_id_ / global_id_        : vertex ID mapping (grow-only)
 ***************************************************************************/
struct CompGraph {
    int nv = 0;
    int edges = 0;
    std::vector<std::vector<int>> adj;
    std::vector<int> tail;
    std::vector<uint8_t> activebit;
    std::vector<uint8_t> mark;
    GroupedStack<int> dropped_verts;
    GroupedStack<int> dropped_tails;

    std::vector<int> local_id_;
    std::vector<int> global_id_;
    int max_vcap_ = 0;
    int max_nv_ = 0;

    inline int deg(int v) { return tail[v]; }

    void build(SubGraph* sg) {
        int vcap = (int)sg->vertex_cap();
        nv = sg->nverts_;
        if (vcap > max_vcap_) { local_id_.resize(vcap, -1); max_vcap_ = vcap; }
        if (nv > max_nv_) {
            global_id_.resize(nv); adj.resize(nv); tail.resize(nv);
            activebit.resize(nv); mark.resize(nv); max_nv_ = nv;
        }
        int idx = 0;
        for (auto v : sg->active_range()) {
            local_id_[(int)v] = idx; global_id_[idx] = (int)v; idx++;
        }
        edges = 0;
        for (int i = 0; i < nv; i++) {
            for (auto nbr_v : sg->Neighs(global_id_[i])) {
                int j = local_id_[(int)nbr_v]; if (j >= 0) mark[j] = 1;
            }
            mark[i] = 1;
            adj[i].clear();
            for (int j = 0; j < nv; j++) if (!mark[j]) adj[i].push_back(j);
            tail[i] = (int)adj[i].size(); edges += tail[i];
            for (auto nbr_v : sg->Neighs(global_id_[i])) {
                int j = local_id_[(int)nbr_v]; if (j >= 0) mark[j] = 0;
            }
            mark[i] = 0;
        }
        edges /= 2;
        for (int i = 0; i < nv; i++) { activebit[i] = 1; mark[i] = 0; }
        for (int i = 0; i < nv; i++) local_id_[global_id_[i]] = -1;
        dropped_verts.clear(); dropped_verts.reserve(nv);
        dropped_tails.clear(); dropped_tails.reserve(nv);
    }

    void drop(int v) {
        activebit[v] = 0;
        for (int i = 0; i < tail[v]; i++) {
            int u = adj[v][i]; if (!activebit[u]) continue;
            int* data = adj[u].data(); int t = tail[u];
            for (int j = 0; j < t; j++) {
                if (data[j] == v) { std::swap(data[j], data[t-1]); tail[u] = t-1; break; }
            }
        }
    }

    void mutate_drop(int v) {
        dropped_verts.push_back(v); dropped_tails.push_back(tail[v]); drop(v);
    }

    void begin_mutate() {
        dropped_verts.create_new_frame(); dropped_tails.create_new_frame();
    }

    void undo_mutate() {
        auto verts = dropped_verts.last_frame_iter();
        auto tails = dropped_tails.last_frame_iter();
        for (int i = (int)verts.size() - 1; i >= 0; i--) {
            int v = verts[i]; activebit[v] = 1; tail[v] = tails[i];
            for (int j = 0; j < tail[v]; j++) {
                int u = adj[v][j]; if (!activebit[u]) continue;
                int* data = adj[u].data(); int t = tail[u];
                int found = -1;
                for (int k = t; k < (int)adj[u].size(); k++) { if (data[k] == v) { found = k; break; } }
                if (found >= 0) { std::swap(data[found], data[t]); tail[u] = t + 1; }
            }
        }
        dropped_verts.pop_frame(); dropped_tails.pop_frame();
    }
};
#endif // ENABLE_IS

#endif  // SUBGRAPH_BIT_H_
