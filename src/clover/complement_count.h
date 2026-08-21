
#ifndef COMPLEMENT_COUNT_H
#define COMPLEMENT_COUNT_H

#include <cstdint>
#include <cstring>
#include <array>
#include <vector>
#include <algorithm>
#include <omp.h>

/*
Clover
Author: <Clover Authors>

Branch-and-reduce solver for independent set for dense subgraphs
- Initially create complement of a subgraph
- Compute the number of independent set within the complement graph
- Returns the number of cliques based on the number of pivots and
  the independent sets counted via polynomial convolution

*/


// IS heuristic — all parameters sweepable via env vars.
// C1: L <= nv <= U && comp_edges < D * nv   (sparse complement trigger)
// C2: density > T/100 && need_k <= 6        (dense + near bottom)
// Env vars: IS_L (default 50), IS_U (default 0 = no limit), IS_D (default 20), IS_T (default 75)
static int _is_param_l = -1;
static int _is_param_u = -1;
static int _is_param_d = -1;
static int _is_param_t = -1;
static void _is_param_init() {
    if (_is_param_l != -1) return;
    const char* e;
    e = getenv("IS_L"); _is_param_l = e ? atoi(e) : 50;
    e = getenv("IS_U"); _is_param_u = e ? atoi(e) : 0;
    e = getenv("IS_D"); _is_param_d = e ? atoi(e) : 20;
    e = getenv("IS_T"); _is_param_t = e ? atoi(e) : 75;
}

namespace complement {

// ── Bitmask: templated on NW words ──

template<int NW> using Bitmask = std::array<uint64_t, NW>;

template<int NW> static int bm_popcount(const Bitmask<NW>& b, int nw) {
    int c = 0; for (int i = 0; i < nw; i++) c += __builtin_popcountll(b[i]); return c;
}
template<int NW> static Bitmask<NW> bm_and(const Bitmask<NW>& a, const Bitmask<NW>& b, int nw) {
    Bitmask<NW> r{}; for (int i = 0; i < nw; i++) r[i] = a[i] & b[i]; return r;
}
template<int NW> static Bitmask<NW> bm_andnot(const Bitmask<NW>& a, const Bitmask<NW>& b, int nw) {
    Bitmask<NW> r{}; for (int i = 0; i < nw; i++) r[i] = a[i] & ~b[i]; return r;
}
template<int NW> static void bm_set(Bitmask<NW>& b, int i) { b[i >> 6] |= (1ULL << (i & 63)); }
template<int NW> static bool bm_test(const Bitmask<NW>& b, int i) { return (b[i >> 6] >> (i & 63)) & 1; }
template<int NW> static void bm_clear(Bitmask<NW>& b, int i) { b[i >> 6] &= ~(1ULL << (i & 63)); }

// Iterate set bits in bitmask — skips empty words
template<int NW, typename Fn>
static void bm_for_each(const Bitmask<NW>& b, int nw, Fn&& fn) {
    for (int w = 0; w < nw; w++) {
        uint64_t bits = b[w];
        while (bits) {
            fn((w << 6) | __builtin_ctzll(bits));
            bits &= bits - 1;
        }
    }
}

static count_t choose(int64_t n, int64_t k) {
    if (k < 0 || k > n) return 0;
    if (k == 0 || k == n) return 1;
    if (k > n - k) k = n - k;
    count_t r = 1;
    for (int64_t i = 0; i < k; i++) r = r * (n - i) / (i + 1);
    return r;
}

template<int NW>
static int count_edges(const Bitmask<NW>* adj, const Bitmask<NW>& active, int nw) {
    int edges = 0;
    bm_for_each<NW>(active, nw, [&](int v) {
        for (int w = (v >> 6); w < nw; w++) {
            uint64_t bits = adj[v][w] & active[w];
            if (w == (v >> 6)) bits &= ~((2ULL << (v & 63)) - 1);
            edges += __builtin_popcountll(bits);
        }
    });
    return edges;
}

// ── Polynomial ──

// MAX_K is the inline-buffer capacity only. It is NOT a bound on k: a polynomial whose window
// is wider than this spills to the heap, so counts stay exact for every k.
#ifndef PS_MAX_K
#define PS_MAX_K 16
#endif
static constexpr int MAX_K = PS_MAX_K;

// Stores exactly [lo_, deg_]; windows wider than INLINE_CAP spill to the heap.
struct Poly {
    static constexpr int INLINE_CAP = MAX_K + 1;   // inline holds INLINE_CAP coefficients
    int      lo_;                                   // lowest stored degree
    int      deg_;                                  // highest stored degree
    count_t* heap_;                                 // nullptr => coefficients live in ibuf_
    count_t  ibuf_[INLINE_CAP];

    Poly() { heap_ = nullptr; lo_ = 0; deg_ = 0; ibuf_[0] = 0; }
    explicit Poly(int deg) { init(0, deg); }
    Poly(int lo, int deg)  { init(lo, deg); }
    Poly(const Poly& o)     { copy_from(o); }
    Poly(Poly&& o) noexcept { move_from(o); }
    Poly& operator=(const Poly& o) {
        if (this == &o) return *this;
        if (!heap_ && !o.heap_) { copy_inline(o); return *this; }
        release(); copy_from(o); return *this;
    }
    Poly& operator=(Poly&& o) noexcept {
        if (this == &o) return *this;
        if (!heap_ && !o.heap_) { copy_inline(o); return *this; }
        release(); move_from(o); return *this;
    }
    ~Poly() { delete[] heap_; }

    int deg() const { return deg_; }
    int lo()  const { return lo_; }
    count_t&       operator[](int i)       { return data()[i - lo_]; }
    const count_t& operator[](int i) const { return data()[i - lo_]; }

  private:
    count_t*       data()       { return heap_ ? heap_ : ibuf_; }
    const count_t* data() const { return heap_ ? heap_ : ibuf_; }
    void copy_inline(const Poly& o) {
        heap_ = nullptr; lo_ = o.lo_; deg_ = o.deg_;
        std::memcpy(ibuf_, o.ibuf_, (size_t)(deg_ - lo_ + 1) * sizeof(count_t));
    }
    void init(int lo, int deg) {
        int w = deg - lo + 1;
        if (w <= INLINE_CAP) { heap_ = nullptr; lo_ = lo; deg_ = deg; std::memset(ibuf_, 0, (size_t)w * sizeof(count_t)); }
        else                 { adopt(new count_t[w](), lo, deg); }
    }
    void adopt(count_t* h, int lo, int deg) { heap_ = h; lo_ = lo; deg_ = deg; }
    void release() { delete[] heap_; heap_ = nullptr; lo_ = 0; deg_ = 0; }
    void copy_from(const Poly& o) {
        if (!o.heap_) { copy_inline(o); }
        else { size_t w = (size_t)(o.deg_ - o.lo_ + 1);
               count_t* h = new count_t[w];
               std::memcpy(h, o.heap_, w * sizeof(count_t));
               adopt(h, o.lo_, o.deg_); }
    }
    void move_from(Poly& o) {
        if (!o.heap_) { copy_inline(o); }
        else { adopt(o.heap_, o.lo_, o.deg_); o.heap_ = nullptr; o.lo_ = 0; o.deg_ = 0; }
    }
};

// ── Coefficient window ──
//
// A fire site consumes only Σ_{j≤num_pivots} C(num_pivots,j)·p[need_total−j], so just the band
// [lo, U] of the polynomial is ever read. Both ends propagate down the recursion, and every
// coefficient outside the band is left at zero instead of being computed.
//
// UPPER END. An independent set of an nv-vertex graph has size ≤ nv, so p[j] = 0 for j > nv and
// a subproblem's degree bound is U = min(maxk, nv)  (poly_deg_bound).
//
// LOWER END (losslessness bound). A coefficient a[i] of one factor reaches output index i plus
// whatever degree the *other* factors can still supply, so
//     lo(child) = band_lo(lo(parent) − remaining_lift)
// where remaining_lift is that maximum contribution from the rest of the expression:
//     branch    result = p_exc + x·p_inc     p_exc: lift 0             p_inc: lift 1
//     iso fold  result = (1+x)^n_iso ⊛ rest  rest:  lift min(n_iso,U)  iso:   lift U(rest)
//     split     result = p1 ⊛ p2             p1:    lift U₂            p2:    lift U₁
// Clamping at 0 is always sound: it only ever asks for coefficients that are not needed.
//
// EMPTY BAND. lo > U means every coefficient the parent could read is genuinely zero (no IS that
// large exists), so the subproblem returns the zero polynomial and its subtree is skipped — the
// top level's `nv + num_pivots < need_total` test, propagated into the recursion.

// U = degree bound of this subproblem's polynomial.
static inline int poly_deg_bound(int maxk, int nv) {
    return maxk < nv ? maxk : nv;
}

// Lower end of a child's needed band; clamping at 0 asks for more than is needed and is sound.
static inline int band_lo(int lo) {
    return lo > 0 ? lo : 0;
}

// Convolve over the output band [lo, maxk]. The result keeps the full 0..maxk degree range even
// though coefficients above deg(a)+deg(b) are zero, so the top-level pivot convolution cannot
// read out of range.
static Poly poly_convolve(const Poly& a, const Poly& b, int maxk, int lo) {
    Poly r(lo, maxk);
    int da = a.deg() < maxk ? a.deg() : maxk;
    for (int i = a.lo(); i <= da; i++)
        if (a[i]) {
            int db = b.deg() < (maxk - i) ? b.deg() : (maxk - i);
            int j0 = lo - i; if (j0 < b.lo()) j0 = b.lo();
            for (int j = j0; j <= db; j++)
                r[i + j] += a[i] * b[j];
        }
    return r;
}

static Poly poly_convolve_binomial(int n, int binom_lo, int binom_deg,
                                   const Poly& b, int maxk, int lo) {
    Poly r(lo, maxk);
    for (int i = binom_lo; i <= binom_deg; i++) {
        count_t a = choose(n, i);
        int db = b.deg() < (maxk - i) ? b.deg() : (maxk - i);
        int j0 = lo - i; if (j0 < b.lo()) j0 = b.lo();
        for (int j = j0; j <= db; j++) r[i + j] += a * b[j];
    }
    return r;
}

// Defined below with the CompGraph path; the near-clique fast path solves its core with it.
static Poly comp_solve(CompGraph& g, int nv, int edges, int maxk, int lo);

// ── BFS to find one connected component ──

template<int NW>
static Bitmask<NW> bfs_component(const Bitmask<NW>* adj, const Bitmask<NW>& active, int n, int nw) {
    int start = -1;
    for (int v = 0; v < n; v++)
        if (bm_test<NW>(active, v)) { start = v; break; }
    if (start < 0) return {};

    Bitmask<NW> visited{}, queue{};
    bm_set<NW>(queue, start);
    bm_set<NW>(visited, start);
    while (bm_popcount<NW>(queue, nw) > 0) {
        Bitmask<NW> next{};
        bm_for_each<NW>(queue, nw, [&](int v) {
            for (int w = 0; w < nw; w++) {
                uint64_t newbits = adj[v][w] & active[w] & ~visited[w];
                next[w] |= newbits;
                visited[w] |= newbits;
            }
        });
        queue = next;
    }
    return visited;
}

// ── Sequential IS counter (used by tasks and deep recursion) ──

template<int NW>
static Poly count_indep_poly(const Bitmask<NW>* adj, Bitmask<NW> active, int n, int nw,
                              int nv, int edges, int maxk, int lo) {
    const int U = poly_deg_bound(maxk, nv);
    if (lo > U) return Poly(U, U);                   // needed band empty: prune the subtree
    if (nv == 0 || maxk == 0) {
        Poly result(lo, U);
        if (lo == 0) result[0] = 1;
        return result;
    }

    // Tighten scan limits: find highest active vertex, reduce n and nw
    for (int w = nw - 1; w >= 0; w--) {
        if (active[w]) { n = (w << 6) + (63 - __builtin_clzll(active[w])) + 1; nw = w + 1; break; }
    }
    if (U == 1) {
        Poly result(lo, U);
        if (lo == 0) result[0] = 1;
        result[1] = nv;
        return result;
    }
    if (edges == 0) {
        Poly result(lo, U);
        for (int j = lo; j <= U && j <= nv; j++) result[j] = choose(nv, j);
        return result;
    }
    if (U == 2) {
        Poly result(lo, U);
        if (lo == 0) result[0] = 1;
        if (lo <= 1) result[1] = nv;
        result[2] = choose(nv, 2) - edges;
        return result;
    }

    // Degree-0 folding: remove isolated vertices, convolve with choose(n_iso, j)
    // Also compute max degree and wedge sum for k=3 termination
    int best = -1, best_deg = -1, n_isolated = 0;
    int64_t wedge_sum = 0;
    Bitmask<NW> isolated{};
    bm_for_each<NW>(active, nw, [&](int v) {
        int deg = bm_popcount<NW>(bm_and<NW>(adj[v], active, nw), nw);
        if (deg == 0) { n_isolated++; bm_set<NW>(isolated, v); }
        if (deg > best_deg) { best_deg = deg; best = v; }
        if (U == 3) wedge_sum += (int64_t)deg * (deg - 1) / 2;
    });
    if (best_deg == 0) {
        Poly result(lo, U);
        for (int j = lo; j <= U && j <= nv; j++) result[j] = choose(nv, j);
        return result;
    }
    if (n_isolated > 0) {
        Bitmask<NW> new_active = bm_andnot<NW>(active, isolated, nw);
        int A  = U < n_isolated ? U : n_isolated;    // effective degree of (1+x)^n_iso here
        int Ur = poly_deg_bound(maxk, nv - n_isolated);
        int iso_lo = band_lo(lo - Ur);
        Poly rest = count_indep_poly<NW>(adj, new_active, n, nw, nv - n_isolated, edges, maxk, band_lo(lo - A));
        return poly_convolve_binomial(n_isolated, iso_lo, A, rest, U, lo);
    }

    // k=3 termination: IS(G,3) = C(n,3) - m*(n-2) + Σ C(deg,2) - triangles
    if (U == 3) {
        int tri = 0;
        bm_for_each<NW>(active, nw, [&](int v) {
            for (int w = (v >> 6); w < nw; w++) {
                uint64_t bits = adj[v][w] & active[w];
                if (w == (v >> 6)) bits &= ~((2ULL << (v & 63)) - 1);
                while (bits) {
                    int u = (w << 6) | __builtin_ctzll(bits);
                    for (int ww = (u >> 6); ww < nw; ww++) {
                        uint64_t common = adj[v][ww] & adj[u][ww] & active[ww];
                        if (ww == (u >> 6)) common &= ~((2ULL << (u & 63)) - 1);
                        tri += __builtin_popcountll(common);
                    }
                    bits &= bits - 1;
                }
            }
        });
        Poly result(lo, U);
        if (lo == 0) result[0] = 1;
        if (lo <= 1) result[1] = nv;
        if (lo <= 2) result[2] = choose(nv, 2) - edges;
        result[3] = choose(nv, 3) - (count_t)edges * (nv - 2) + (count_t)wedge_sum - tri;
        return result;
    }

    // Connected components decomposition (skip BFS for nv<=16, rarely disconnected)
    if (nv > 16) {
    Bitmask<NW> comp1 = bfs_component<NW>(adj, active, n, nw);
    int comp1_size = bm_popcount<NW>(comp1, nw);
    if (comp1_size < nv) {
        Bitmask<NW> rest = bm_andnot<NW>(active, comp1, nw);
        int rest_size = nv - comp1_size;
        int edges1 = count_edges<NW>(adj, comp1, nw);
        int U1 = poly_deg_bound(maxk, comp1_size), U2 = poly_deg_bound(maxk, rest_size);
        Poly p1 = count_indep_poly<NW>(adj, comp1, n, nw, comp1_size, edges1, maxk, band_lo(lo - U2));
        Poly p2 = count_indep_poly<NW>(adj, rest, n, nw, rest_size, edges - edges1, maxk, band_lo(lo - U1));
        return poly_convolve(p1, p2, U, lo);
    }
    } // end nv > 16

    // Branch on max-degree vertex (best/best_deg computed in pre-scan above)

    // Include: remove vertex + all neighbors
    Bitmask<NW> nbrs = bm_and<NW>(adj[best], active, nw);
    Bitmask<NW> active1 = bm_andnot<NW>(active, nbrs, nw);
    bm_clear<NW>(active1, best);
    int nv1 = nv - 1 - bm_popcount<NW>(nbrs, nw);
    Bitmask<NW> removed = nbrs;
    bm_set<NW>(removed, best);
    int total_deg = 0, internal = 0;
    bm_for_each<NW>(removed, nw, [&](int v) {
        total_deg += bm_popcount<NW>(bm_and<NW>(adj[v], active, nw), nw);
        for (int w = (v >> 6); w < nw; w++) {
            uint64_t bits = adj[v][w] & removed[w];
            if (w == (v >> 6)) bits &= ~((2ULL << (v & 63)) - 1);
            internal += __builtin_popcountll(bits);
        }
    });
    int edges1 = edges - total_deg + internal;

    // Exclude: remove just the vertex
    Bitmask<NW> active2 = active;
    bm_clear<NW>(active2, best);

    Poly p_inc = count_indep_poly<NW>(adj, active1, n, nw, nv1, edges1, maxk - 1, band_lo(lo - 1));
    Poly p_exc = count_indep_poly<NW>(adj, active2, n, nw, nv - 1, edges - best_deg, maxk, lo);

    // Each child's own vertex count can bound it below U, so copy to its own degree.
    Poly result(lo, U);
    int de = p_exc.deg() < U ? p_exc.deg() : U;
    for (int j = lo; j <= de; j++) result[j] = p_exc[j];
    int di = p_inc.deg() < U - 1 ? p_inc.deg() : U - 1;
    for (int j = (lo > 0 ? lo - 1 : 0); j <= di; j++) result[j + 1] += p_inc[j];
    return result;
}

// ── Work-list parallel IS: pre-walk exclude chain, parallel-for branches ──

template<int NW>
struct WorkItem {
    Bitmask<NW> active;
    int nv;
    int edges;
    int maxk;
    int lo;
};

template<int NW>
static Poly count_indep_parallel(const Bitmask<NW>* adj, Bitmask<NW> active,
                                  int n, int nw, int nv, int edges, int maxk, int lo) {
    if (lo > poly_deg_bound(maxk, nv))
        return Poly(poly_deg_bound(maxk, nv), poly_deg_bound(maxk, nv));

    // Pre-walk exclude chain: collect include branches as work items
    std::vector<WorkItem<NW>> branches;
    branches.reserve(nv);

    Bitmask<NW> cur_active = active;
    int cur_nv = nv, cur_edges = edges;

    while (cur_nv > maxk && cur_edges > 0 && maxk > 2) {
        // Check connectivity
        Bitmask<NW> comp1 = bfs_component<NW>(adj, cur_active, n, nw);
        int comp1_size = bm_popcount<NW>(comp1, nw);
        if (comp1_size < cur_nv) break; // disconnected — fall back to recursive

        // Pick max-degree vertex
        int best = -1, best_deg = -1;
        for (int v = 0; v < n; v++) {
            if (!bm_test<NW>(cur_active, v)) continue;
            int deg = bm_popcount<NW>(bm_and<NW>(adj[v], cur_active, nw), nw);
            if (deg > best_deg) { best_deg = deg; best = v; }
        }
        if (best_deg == 0) break;

        // Record include branch
        Bitmask<NW> nbrs = bm_and<NW>(adj[best], cur_active, nw);
        Bitmask<NW> inc_active = bm_andnot<NW>(cur_active, nbrs, nw);
        bm_clear<NW>(inc_active, best);
        int inc_nv = cur_nv - 1 - bm_popcount<NW>(nbrs, nw);

        Bitmask<NW> removed = nbrs; bm_set<NW>(removed, best);
        int total_deg = 0;
        for (int v = 0; v < n; v++) {
            if (!bm_test<NW>(removed, v)) continue;
            total_deg += bm_popcount<NW>(bm_and<NW>(adj[v], cur_active, nw), nw);
        }
        int internal = 0;
        for (int v = 0; v < n; v++) {
            if (!bm_test<NW>(removed, v)) continue;
            for (int w = (v >> 6); w < nw; w++) {
                uint64_t bits = adj[v][w] & removed[w];
                if (w == (v >> 6)) bits &= ~((2ULL << (v & 63)) - 1);
                internal += __builtin_popcountll(bits);
            }
        }
        int inc_edges = cur_edges - total_deg + internal;

        // Every include branch shifts by exactly one, so each needs band [lo-1, ·]; a branch
        // whose own degree bound falls below that contributes only zeros and is not queued.
        int inc_lo = band_lo(lo - 1);
        if (inc_lo <= poly_deg_bound(maxk - 1, inc_nv))
            branches.push_back({inc_active, inc_nv, inc_edges, maxk - 1, inc_lo});

        // Exclude: continue chain
        bm_clear<NW>(cur_active, best);
        cur_edges -= best_deg;
        cur_nv--;
    }

    // Solve remainder of chain (disconnected or base case)
    Poly chain_poly = count_indep_poly<NW>(adj, cur_active, n, nw, cur_nv, cur_edges, maxk, lo);

    if (branches.empty()) return chain_poly;

    // Process include branches in parallel via OMP tasks
    int nbranches = (int)branches.size();
    std::vector<Poly> branch_results(nbranches);

    for (int i = 0; i < nbranches; i++) {
        #pragma omp task shared(branch_results, branches, adj) firstprivate(i, n, nw)
        {
            const WorkItem<NW>& w = branches[i];
            branch_results[i] = count_indep_poly<NW>(adj, w.active, n, nw, w.nv, w.edges, w.maxk, w.lo);
        }
    }
    #pragma omp taskwait

    // Combine: each include branch shifts poly by 1 (one vertex included).
    // Every poly carries its own degree bound ≤ U, so each copy stops at that bound.
    const int U = poly_deg_bound(maxk, nv);
    Poly result(lo, U);
    int dc = chain_poly.deg() < U ? chain_poly.deg() : U;
    for (int j = lo; j <= dc; j++) result[j] = chain_poly[j];
    int jb = lo > 0 ? lo - 1 : 0;
    for (int i = 0; i < nbranches; i++) {
        const Poly& b = branch_results[i];
        int db = b.deg() < U - 1 ? b.deg() : U - 1;
        for (int j = jb; j <= db; j++) result[j + 1] += b[j];
    }
    return result;
}

// ── Build complement and count IS ──

// Below this nv the O(nv²/64) dense build no longer dominates the O(nv) fast-path prescan, so
// the prescan is pure overhead — and tiny subgraphs are most of the search forest. The crossover
// follows from build-vs-prescan cost alone, not from any graph.
static constexpr int FASTPATH_MIN_NV = 64;

// Fires when the induced subgraph is complete: the complement is empty, so its IS polynomial is
// (1+x)^nv, i.e. p[d] = C(nv,d). Substituting that into the full path's
// Σ_j C(num_pivots,j)·p[need_total−j] over the same index range gives an identical result with
// neither the O(nv²) complement build nor the solver.
static inline count_t complete_is_convolve(int nv, int num_pivots, int need_total) {
    int poly_k = need_total < nv ? need_total : nv;
    count_t result = 0;
    for (int j = 0; j <= num_pivots && j <= need_total; j++) {
        int from_sg = need_total - j;
        if (from_sg >= 0 && from_sg <= poly_k)
            result += choose(num_pivots, j) * choose(nv, from_sg);
    }
    return result;
}

// One O(nv) pass over induced degrees, feeding BOTH fast paths. A vertex is "universal" iff it
// has nv−1 induced neighbours (complement-degree 0); the rest form the non-isolated core S of
// the complement, and complete == (s == 0). Two structural facts both paths rely on:
//   • non-adjacency is symmetric, so every complement edge has both endpoints in S — hence
//     |S| ∈ {0} ∪ {≥2} and Σ_{v∈S} complement-degree(v) = 2m;
//   • R = Σ_{v∈S} induced-degree(v) = s·(nv−1) − 2m is the sparse build's neighbour-read cost.
struct CompPrescan {
    bool complete;
    int  s;                    // |S|, non-universal vertices
    int  n_iso;                // nv − s, universal vertices (isolated in the complement)
    int64_t m;                 // complement edge count
    int64_t R;                 // Σ_{v∈S} induced-degree (sparse-build read cost)
    std::vector<int>* Svec;    // thread-local list of the s core vertices (sg-local ids)
};

template<typename SubGraph>
static CompPrescan comp_prescan(const SubGraph* sg, int nv) {
    static thread_local std::vector<int> Sbuf;
    Sbuf.clear();
    int64_t twoM = 0, R = 0;
    for (auto v : sg->active_range()) {
        int d = (int)sg->Neighs(v).size();
        int c = (nv - 1) - d;                       // complement degree
        if (c > 0) { Sbuf.push_back((int)v); R += d; twoM += c; }
    }
    CompPrescan p;
    p.s = (int)Sbuf.size();
    p.complete = (p.s == 0);
    p.n_iso = nv - p.s;
    p.m = twoM / 2;
    p.R = R;
    p.Svec = &Sbuf;
    return p;
}

// Near-complete (sparse-complement) fast path: build the complement restricted to the core S
// instead of over all nv vertices — read each core vertex's list once to mark its intra-S
// neighbours (Σ = R), scan [0,s) to collect its complement neighbours, clear the marks (2·s per
// vertex) — then fold the universal vertices back analytically:
//     I(complement) = (1+x)^{n_iso} · I(complement[S])
// so p[d] = Σ_i C(n_iso,i)·q[d−i] with q the core's independence polynomial.
//
// Identical to the dense path because complement[S] IS the core its degree-0 fold reduces to,
// and the pivot convolution runs over the same index range. s ≥ 2 whenever s > 0, so
// ks = min(poly_k,s) ≥ 1.
template<typename SubGraph>
static count_t nearclique_is_convolve(SubGraph* sg, const CompPrescan& st,
                                      int nv, int num_pivots, int need_total) {
    static thread_local CompGraph gS;
    static thread_local std::vector<int> loc;      // sg-local id → [0,s), else −1
    const std::vector<int>& S = *st.Svec;
    int s = st.s;

    int vcap = (int)sg->vertex_cap();
    if ((int)loc.size() < vcap) loc.resize(vcap, -1);
    if (s > gS.max_nv_) {
        gS.global_id_.resize(s); gS.adj.resize(s); gS.tail.resize(s);
        gS.activebit.resize(s);  gS.mark.resize(s); gS.max_nv_ = s;
    }
    for (int i = 0; i < s; i++) loc[S[i]] = i;

    int64_t e2 = 0;
    for (int i = 0; i < s; i++) {
        for (auto nbr : sg->Neighs(S[i])) { int j = loc[(int)nbr]; if (j >= 0) gS.mark[j] = 1; }
        gS.mark[i] = 1;                             // exclude self
        gS.adj[i].clear();
        for (int j = 0; j < s; j++) if (!gS.mark[j]) gS.adj[i].push_back(j);
        gS.tail[i] = (int)gS.adj[i].size(); e2 += gS.tail[i];
        for (int j = 0; j < s; j++) gS.mark[j] = 0;  // restore mark to all-zero for comp_solve
    }
    gS.nv = s;
    gS.edges = (int)(e2 / 2);
    for (int i = 0; i < s; i++) gS.activebit[i] = 1;
    for (int i = 0; i < s; i++) loc[S[i]] = -1;
    gS.dropped_verts.clear(); gS.dropped_verts.reserve(s);
    gS.dropped_tails.clear(); gS.dropped_tails.reserve(s);

    int poly_k = need_total < nv ? need_total : nv;
    int ks     = poly_k < s ? poly_k : s;
    int lo0    = band_lo(need_total - num_pivots);
    int isod   = poly_k < st.n_iso ? poly_k : st.n_iso;

    // The universal vertices fold in as (1+x)^n_iso, so the core's band starts isod lower.
    Poly q = comp_solve(gS, s, gS.edges, ks, band_lo(lo0 - isod));

    Poly p = poly_convolve_binomial(st.n_iso, band_lo(lo0 - ks), isod, q, poly_k, lo0);

    count_t result = 0;
    for (int j = 0; j <= num_pivots && j <= need_total; j++) {
        int from_sg = need_total - j;
        if (from_sg >= 0 && from_sg <= p.deg())
            result += choose(num_pivots, j) * p[from_sg];
    }
    return result;
}

// Fires only when the sparse build is strictly cheaper than the dense one, counted in memory
// touches (per-touch costs are equal, and the IS recursion plus the iso convolution are paid by
// both paths, so they cancel and do not appear):
//   sparse = R + 2·s²            read S lists once, then two [0,s) scans per core vertex
//   dense  = 2·nedges + nv²      CompGraph: mark+unmark reads + the j∈[0,nv) inner scan
//   dense  = present + 2·nv·nw   bitmask nv≤512: has_neigh build + complement-NOT + count_edges
// present = nv·(nv−1) − 2m is derived from the prescan's m, not SubGraph::nedges_, because that
// field is not restored by UndoSelfMutate and can be stale at a count_cliques entry.
// The 2·s² term is what keeps the path off dense-but-not-near-complete subgraphs (s ≈ nv), where
// sparse ≈ dense and the strict "<" fails.
static inline bool nearclique_fire(int64_t sparse_touches, int64_t dense_touches, int s) {
    return s > 0 && sparse_touches < dense_touches;
}

template<int NW, int MAX_V>
struct CountImpl {
    template<typename SubGraph>
    static count_t run(SubGraph* sg, int max_k, int clique_size, int num_pivots) {
        int nv = sg->nverts_;
        int num_holds = clique_size - num_pivots;
        int need_total = max_k - num_holds;
        if (need_total <= 0) return 0;
        if (nv + num_pivots < need_total) return 0;

        // Both fast paths share one O(nv) prescan, gated by nv (see FASTPATH_MIN_NV).
        if (nv >= FASTPATH_MIN_NV) {
            CompPrescan st = comp_prescan(sg, nv);
            if (st.complete) return complete_is_convolve(nv, num_pivots, need_total);
            // Bitmask dense build cost: read has_neigh (present) + complement-NOT and count_edges.
            int nw_bm = (nv + 63) / 64;
            int64_t present = (int64_t)nv * (nv - 1) - 2 * st.m;
            int64_t sparse_touches = st.R + 2 * (int64_t)st.s * st.s;
            int64_t dense_touches  = present + 2 * (int64_t)nv * nw_bm;
            if (nearclique_fire(sparse_touches, dense_touches, st.s))
                return nearclique_is_convolve(sg, st, nv, num_pivots, need_total);
        }

        int vcap = (int)sg->vertex_cap();
        std::vector<int> local_id(vcap, -1);
        std::vector<int> global_id(nv);
        int idx = 0;
        for (auto v : sg->active_range()) {
            if (idx >= MAX_V) return 0;
            local_id[(int)v] = idx;
            global_id[idx] = (int)v;
            idx++;
        }
        int nw = (nv + 63) / 64;

        // Build active mask
        Bitmask<NW> active{};
        for (int i = 0; i < nv; i++) bm_set<NW>(active, i);

        // Build neighbor bitmask from SubGraph
        std::vector<Bitmask<NW>> has_neigh(nv);   // value-initialized, i.e. all bits clear
        for (int i = 0; i < nv; i++) {
            for (auto nbr : sg->Neighs(global_id[i])) {
                int j = local_id[(int)nbr];
                if (j >= 0) bm_set<NW>(has_neigh[i], j);
            }
        }

        // Complement via bitmask NOT
        std::vector<Bitmask<NW>> adj_store(nv);
        for (int i = 0; i < nv; i++) {
            adj_store[i] = bm_andnot<NW>(active, has_neigh[i], nw);
            bm_clear<NW>(adj_store[i], i);
        }
        int edges = count_edges<NW>(adj_store.data(), active, nw);
        // Bounded by nv: an IS larger than nv is empty, which keeps deep high-k pivot chains
        // (nv collapses while need_total stays put) small.
        int poly_k = need_total < nv ? need_total : nv;

        // Consumed band: Σ_j C(num_pivots,j)·p[need_total−j] reads only [lo0, poly_k].
        int lo0 = band_lo(need_total - num_pivots);

        // Parallel work-list for large problems, sequential for small
        Poly p;
        if (nv > 100 && poly_k > 3)
            p = count_indep_parallel<NW>(adj_store.data(), active, nv, nw, nv, edges, poly_k, lo0);
        else
            p = count_indep_poly<NW>(adj_store.data(), active, nv, nw, nv, edges, poly_k, lo0);

        // Convolve IS polynomial with pivot polynomial: Σ C(num_pivots, j) * p[need_total - j]
        count_t result = 0;
        for (int j = 0; j <= num_pivots && j <= need_total; j++) {
            int from_sg = need_total - j;
            if (from_sg >= 0 && from_sg <= p.deg())
                result += choose(num_pivots, j) * p[from_sg];
        }
        return result;
    }
};

// CompGraph defined in subgraph.h (alongside SubGraph)

// IS solver on CompGraph (in-place mutation + undo)
static Poly comp_solve(CompGraph& g, int nv, int edges, int maxk, int lo) {
    const int U = poly_deg_bound(maxk, nv);
    if (lo > U) return Poly(U, U);                   // needed band empty: prune the subtree
    if (nv == 0 || maxk == 0) {
        Poly result(lo, U);
        if (lo == 0) result[0] = 1;
        return result;
    }
    if (U == 1) {
        Poly result(lo, U);
        if (lo == 0) result[0] = 1;
        result[1] = nv;
        return result;
    }
    if (edges == 0) {
        Poly result(lo, U);
        for (int j = lo; j <= U && j <= nv; j++) result[j] = choose(nv, j);
        return result;
    }
    if (U == 2) {
        Poly result(lo, U);
        if (lo == 0) result[0] = 1;
        if (lo <= 1) result[1] = nv;
        result[2] = choose(nv, 2) - edges;
        return result;
    }

    // Pre-scan: iterate active vertices via activebit
    int best = -1, best_deg = -1, n_isolated = 0;
    int64_t wedge_sum = 0;
    for (int v = 0; v < g.nv; v++) {
        if (!g.activebit[v]) continue;
        int d = g.deg(v);
        if (d == 0) n_isolated++;
        if (d > best_deg) { best_deg = d; best = v; }
        wedge_sum += (int64_t)d * (d-1) / 2;
    }

    // Degree-0 folding
    if (n_isolated > 0) {
        g.begin_mutate();
        for (int v = 0; v < g.nv; v++) {
            if (!g.activebit[v]) continue;
            if (g.deg(v) == 0) g.mutate_drop(v);
        }
        int A  = U < n_isolated ? U : n_isolated;
        int Ur = poly_deg_bound(maxk, nv - n_isolated);
        int iso_lo = band_lo(lo - Ur);
        Poly rest = comp_solve(g, nv - n_isolated, edges, maxk, band_lo(lo - A));
        g.undo_mutate();
        return poly_convolve_binomial(n_isolated, iso_lo, A, rest, U, lo);
    }

    // k=3 triangle formula
    if (U == 3) {
        int tri = 0;
        for (int v = 0; v < g.nv; v++) {
            if (!g.activebit[v]) continue;
            for (int i = 0; i < g.tail[v]; i++) {
                int u = g.adj[v][i];
                if (u <= v) continue;
                // MoveToTail leaves adj unsorted, so common neighbours come from marking.
                for (int j = 0; j < g.tail[u]; j++) g.mark[g.adj[u][j]] = 1;
                for (int j = 0; j < g.tail[v]; j++) {
                    int w = g.adj[v][j];
                    if (w > u && g.mark[w]) tri++;
                }
                for (int j = 0; j < g.tail[u]; j++) g.mark[g.adj[u][j]] = 0;
            }
        }
        Poly result(lo, U);
        if (lo == 0) result[0] = 1;
        if (lo <= 1) result[1] = nv;
        if (lo <= 2) result[2] = choose(nv, 2) - edges;
        result[3] = choose(nv,3) - (count_t)edges*(nv-2) + (count_t)wedge_sum - tri;
        return result;
    }

    // BFS connected components
    if (nv > 16) {
        int start = -1;
        for (int v = 0; v < g.nv; v++) if (g.activebit[v]) { start = v; break; }
        g.mark[start] = 1;
        // BFS using a local vector (small, one per component split)
        std::vector<int> comp1;
        comp1.reserve(nv);
        comp1.push_back(start);
        int qh = 0;
        while (qh < (int)comp1.size()) {
            int v = comp1[qh++];
            for (int i = 0; i < g.tail[v]; i++) {
                int w = g.adj[v][i];
                if (!g.mark[w]) { g.mark[w] = 1; comp1.push_back(w); }
            }
        }
        int c1sz = (int)comp1.size();
        for (int v : comp1) g.mark[v] = 0;

        if (c1sz < nv) {
            // Drop rest (non-comp1)
            for (int v : comp1) g.mark[v] = 1;
            g.begin_mutate();
            for (int v = 0; v < g.nv; v++) {
                if (g.activebit[v] && !g.mark[v]) g.mutate_drop(v);
            }
            for (int v : comp1) g.mark[v] = 0;

            int e1 = 0;
            for (int v : comp1) e1 += g.deg(v);
            e1 /= 2;
            int U1 = poly_deg_bound(maxk, c1sz), U2 = poly_deg_bound(maxk, nv - c1sz);
            Poly p1 = comp_solve(g, c1sz, e1, maxk, band_lo(lo - U2));
            g.undo_mutate();

            // Drop comp1
            g.begin_mutate();
            for (int v : comp1) g.mutate_drop(v);
            Poly p2 = comp_solve(g, nv - c1sz, edges - e1, maxk, band_lo(lo - U1));
            g.undo_mutate();

            return poly_convolve(p1, p2, U, lo);
        }
    }

    // Branch on max-degree vertex — inline, NO storage for neighbors

    // Edge counting: mark removed set
    g.mark[best] = 1;
    for (int i = 0; i < g.tail[best]; i++) g.mark[g.adj[best][i]] = 1;
    int total_deg = best_deg, iloop = 0;
    for (int i = 0; i < g.tail[best]; i++) {
        int u = g.adj[best][i];
        for (int j = 0; j < g.tail[u]; j++) {
            total_deg++;
            if (g.mark[g.adj[u][j]]) iloop++;
        }
    }
    int internal = (iloop + best_deg) / 2;
    g.mark[best] = 0;
    for (int i = 0; i < g.tail[best]; i++) g.mark[g.adj[best][i]] = 0;

    int edges1 = edges - total_deg + internal;
    int nv1 = nv - 1 - best_deg;

    // Include: drop best + all active neighbors
    // drop(best) doesn't modify adj[best]'s data, only other vertices' tails.
    // So we can iterate adj[best][0..best_deg) after dropping best.
    g.begin_mutate();
    int nbrs_to_drop = g.tail[best]; // save before drop
    g.mutate_drop(best);
    for (int i = 0; i < nbrs_to_drop; i++) {
        int u = g.adj[best][i];
        if (g.activebit[u]) g.mutate_drop(u);
    }
    Poly p_inc = comp_solve(g, nv1, edges1, maxk - 1, band_lo(lo - 1));
    g.undo_mutate();

    // Exclude: drop best only
    g.begin_mutate();
    g.mutate_drop(best);
    Poly p_exc = comp_solve(g, nv - 1, edges - best_deg, maxk, lo);
    g.undo_mutate();

    Poly result(lo, U);
    int de = p_exc.deg() < U ? p_exc.deg() : U;
    for (int j = lo; j <= de; j++) result[j] = p_exc[j];
    int di = p_inc.deg() < U - 1 ? p_inc.deg() : U - 1;
    for (int j = (lo > 0 ? lo - 1 : 0); j <= di; j++) result[j + 1] += p_inc[j];
    return result;
}

// Build complement + solve for nv > 512
struct CompCountImpl {
    template<typename SubGraph>
    static count_t run(SubGraph* sg, int max_k, int clique_size, int num_pivots) {
        int nv = sg->nverts_;
        int num_holds = clique_size - num_pivots;
        int need_total = max_k - num_holds;
        if (need_total <= 0) return 0;
        if (nv + num_pivots < need_total) return 0;

        // Both fast paths share one O(nv) prescan; nv > 512 here, so no nv gate is needed.
        CompPrescan st = comp_prescan(sg, nv);
        if (st.complete) return complete_is_convolve(nv, num_pivots, need_total);
        // CompGraph dense build cost: mark+unmark reads (2·present) + the j∈[0,nv) inner scan.
        int64_t present = (int64_t)nv * (nv - 1) - 2 * st.m;   // Σ deg = directed present-edge sum
        int64_t sparse_touches = st.R + 2 * (int64_t)st.s * st.s;
        int64_t dense_touches  = 2 * present + (int64_t)nv * nv;
        if (nearclique_fire(sparse_touches, dense_touches, st.s))
            return nearclique_is_convolve(sg, st, nv, num_pivots, need_total);

        // Per-thread CompGraph: allocated once, reused across calls
        static thread_local CompGraph g;
        g.build(sg);
        // Degree bounded by nv (see CountImpl): an IS larger than g.nv is empty.
        int poly_k = need_total < g.nv ? need_total : g.nv;
        int lo0 = band_lo(need_total - num_pivots);
        Poly p = comp_solve(g, g.nv, g.edges, poly_k, lo0);

        count_t result = 0;
        for (int j = 0; j <= num_pivots && j <= need_total; j++) {
            int from_sg = need_total - j;
            if (from_sg >= 0 && from_sg <= p.deg())
                result += choose(num_pivots, j) * p[from_sg];
        }
        return result;
    }
};

// ── Public API ──

template<typename SubGraph>
static bool should_use(const SubGraph* sg, int max_k, int clique_size, int num_pivots) {
    _is_param_init();
    int nv = sg->nverts_;

    int64_t max_edges = (int64_t)nv * (nv - 1);
    int64_t complement_edges = (max_edges - sg->nedges_) / 2;

    // C1: sparse complement (complement edges per vertex < D)
    if (complement_edges < (int64_t)_is_param_d * nv)
        return true;

    // C2: dense graph + near bottom of recursion
    int need_k = max_k - (clique_size - num_pivots);
    if ((int64_t)sg->nedges_ * 100 > (int64_t)_is_param_t * max_edges && need_k <= 6)
        return true;

    return false;
}

template<typename SubGraph>
static count_t count_cliques(SubGraph* sg, int max_k, int clique_size, int num_pivots) {
    int nv = sg->nverts_;

    // Hybrid: bitmask for nv≤512 (8 words, fits cache, vectorized AND/popcount)
    // CompGraph (adj_list + MoveToTail) for nv>512 (no vertex limit, O(active_deg) iteration)
    if (nv <= 512)
        return CountImpl<8, 512>::run(sg, max_k, clique_size, num_pivots);
    else
        return CompCountImpl::run(sg, max_k, clique_size, num_pivots);
}

} // namespace complement

#endif // COMPLEMENT_COUNT_H
