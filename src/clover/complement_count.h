// complement_count.h — Complement-based independent set counting for dense subgraphs.
// Templated on NW (bitmask words): NW=8 for ≤512 vertices, NW=64 for ≤4096 vertices.
// All-pivot support via polynomial convolution with C(num_pivots, j).
// Parallel IS via work-list: pre-walk exclude chain, parallel-for include branches.


#ifndef COMPLEMENT_COUNT_H
#define COMPLEMENT_COUNT_H

#include <cstdint>
#include <array>
#include <vector>
#include <algorithm>
#include <omp.h>

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
static int count_edges(const Bitmask<NW>* adj, const Bitmask<NW>& active, int n, int nw) {
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

static constexpr int MAX_K = 16;
using Poly = std::array<count_t, MAX_K + 1>;

static Poly poly_convolve(const Poly& a, const Poly& b, int maxk) {
    Poly r{};
    for (int i = 0; i <= maxk; i++)
        if (a[i]) for (int j = 0; j <= maxk - i; j++)
            r[i + j] += a[i] * b[j];
    return r;
}

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
                              int nv, int edges, int maxk) {
    Poly result{};
    result[0] = 1;
    if (nv == 0 || maxk == 0) return result;

    // Tighten scan limits: find highest active vertex, reduce n and nw
    for (int w = nw - 1; w >= 0; w--) {
        if (active[w]) { n = (w << 6) + (63 - __builtin_clzll(active[w])) + 1; nw = w + 1; break; }
    }
    result[1] = nv;
    if (maxk == 1) return result;
    if (edges == 0) {
        for (int j = 0; j <= maxk && j <= nv; j++) result[j] = choose(nv, j);
        return result;
    }
    result[2] = choose(nv, 2) - edges;
    if (maxk == 2) return result;

    // Degree-0 folding: remove isolated vertices, convolve with choose(n_iso, j)
    // Also compute max degree and wedge sum for k=3 termination
    int best = -1, best_deg = -1, n_isolated = 0;
    int64_t wedge_sum = 0;
    bm_for_each<NW>(active, nw, [&](int v) {
        int deg = bm_popcount<NW>(bm_and<NW>(adj[v], active, nw), nw);
        if (deg == 0) n_isolated++;
        if (deg > best_deg) { best_deg = deg; best = v; }
        wedge_sum += (int64_t)deg * (deg - 1) / 2;
    });
    if (best_deg == 0) {
        for (int j = 0; j <= maxk && j <= nv; j++) result[j] = choose(nv, j);
        return result;
    }
    if (n_isolated > 0) {
        Bitmask<NW> new_active = active;
        bm_for_each<NW>(active, nw, [&](int v) {
            if (bm_popcount<NW>(bm_and<NW>(adj[v], active, nw), nw) == 0)
                bm_clear<NW>(new_active, v);
        });
        Poly iso{};
        for (int j = 0; j <= maxk && j <= n_isolated; j++) iso[j] = choose(n_isolated, j);
        Poly rest = count_indep_poly<NW>(adj, new_active, n, nw, nv - n_isolated, edges, maxk);
        return poly_convolve(iso, rest, maxk);
    }

    // k=3 termination: IS(G,3) = C(n,3) - m*(n-2) + Σ C(deg,2) - triangles
    if (maxk == 3) {
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
        int edges1 = count_edges<NW>(adj, comp1, n, nw);
        Poly p1 = count_indep_poly<NW>(adj, comp1, n, nw, comp1_size, edges1, maxk);
        Poly p2 = count_indep_poly<NW>(adj, rest, n, nw, rest_size, edges - edges1, maxk);
        return poly_convolve(p1, p2, maxk);
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

    Poly p_inc = count_indep_poly<NW>(adj, active1, n, nw, nv1, edges1, maxk - 1);
    Poly p_exc = count_indep_poly<NW>(adj, active2, n, nw, nv - 1, edges - best_deg, maxk);

    for (int j = 0; j <= maxk; j++) {
        result[j] = p_exc[j];
        if (j > 0) result[j] += p_inc[j - 1];
    }
    return result;
}

// ── Work-list parallel IS: pre-walk exclude chain, parallel-for branches ──

template<int NW>
struct WorkItem {
    Bitmask<NW> active;
    int nv;
    int edges;
    int maxk;
};

template<int NW>
static Poly count_indep_parallel(const Bitmask<NW>* adj, Bitmask<NW> active,
                                  int n, int nw, int nv, int edges, int maxk) {
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

        branches.push_back({inc_active, inc_nv, inc_edges, maxk - 1});


        // Exclude: continue chain
        bm_clear<NW>(cur_active, best);
        cur_edges -= best_deg;
        cur_nv--;
    }

    // Solve remainder of chain (disconnected or base case)
    Poly chain_poly = count_indep_poly<NW>(adj, cur_active, n, nw, cur_nv, cur_edges, maxk);

    if (branches.empty()) return chain_poly;

    // Process include branches in parallel via OMP tasks
    int nbranches = (int)branches.size();
    std::vector<Poly> branch_results(nbranches);

    for (int i = 0; i < nbranches; i++) {
        #pragma omp task shared(branch_results, branches, adj) firstprivate(i, n, nw)
        {
            const WorkItem<NW>& w = branches[i];
            branch_results[i] = count_indep_poly<NW>(adj, w.active, n, nw, w.nv, w.edges, w.maxk);
        }
    }
    #pragma omp taskwait

    // Combine: each include branch shifts poly by 1 (one vertex included)
    Poly result = chain_poly;
    for (int i = 0; i < nbranches; i++) {
        for (int j = 1; j <= maxk; j++) {
            result[j] += branch_results[i][j - 1];
        }
    }
    return result;
}

// ── Build complement and count IS ──

template<int NW, int MAX_V>
struct CountImpl {
    template<typename SubGraph>
    static count_t run(SubGraph* sg, int max_k, int clique_size, int num_pivots) {
        int nv = sg->nverts_;
        int num_holds = clique_size - num_pivots;
        int need_total = max_k - num_holds;
        if (need_total <= 0) return 0;
        if (nv + num_pivots < need_total) return 0;

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
        std::vector<Bitmask<NW>> has_neigh(nv);
        for (int i = 0; i < nv; i++) has_neigh[i] = {};
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
        int edges = count_edges<NW>(adj_store.data(), active, nv, nw);
        int poly_k = (need_total <= MAX_K) ? need_total : MAX_K;

        // Parallel work-list for large problems, sequential for small
        Poly p;
        if (nv > 100 && poly_k > 3)
            p = count_indep_parallel<NW>(adj_store.data(), active, nv, nw, nv, edges, poly_k);
        else
            p = count_indep_poly<NW>(adj_store.data(), active, nv, nw, nv, edges, poly_k);

        // Convolve IS polynomial with pivot polynomial: Σ C(num_pivots, j) * p[need_total - j]
        count_t result = 0;
        for (int j = 0; j <= num_pivots && j <= need_total; j++) {
            int from_sg = need_total - j;
            if (from_sg >= 0 && from_sg <= poly_k)
                result += choose(num_pivots, j) * p[from_sg];
        }
        return result;
    }
};

// CompGraph defined in subgraph.h (alongside SubGraph)

// IS solver on CompGraph (in-place mutation + undo)
static Poly comp_solve(CompGraph& g, int nv, int edges, int maxk) {
    Poly result{};
    result[0] = 1;
    if (nv == 0 || maxk == 0) return result;
    result[1] = nv;
    if (maxk == 1) return result;
    if (edges == 0) {
        for (int j = 0; j <= maxk && j <= nv; j++) result[j] = choose(nv, j);
        return result;
    }
    result[2] = choose(nv, 2) - edges;
    if (maxk == 2) return result;

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
        Poly iso{}; for (int j = 0; j <= maxk && j <= n_isolated; j++) iso[j] = choose(n_isolated, j);
        Poly rest = comp_solve(g, nv - n_isolated, edges, maxk);
        g.undo_mutate();
        return poly_convolve(iso, rest, maxk);
    }

    // k=3 triangle formula
    if (maxk == 3) {
        int tri = 0;
        for (int v = 0; v < g.nv; v++) {
            if (!g.activebit[v]) continue;
            for (int i = 0; i < g.tail[v]; i++) {
                int u = g.adj[v][i];
                if (u <= v) continue;
                // Sorted merge for common neighbors w > u
                // But adj lists are NOT sorted after MoveToTail swaps!
                // Use mark instead: mark u's neighbors, scan v's neighbors
                for (int j = 0; j < g.tail[u]; j++) g.mark[g.adj[u][j]] = 1;
                for (int j = 0; j < g.tail[v]; j++) {
                    int w = g.adj[v][j];
                    if (w > u && g.mark[w]) tri++;
                }
                for (int j = 0; j < g.tail[u]; j++) g.mark[g.adj[u][j]] = 0;
            }
        }
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
            Poly p1 = comp_solve(g, c1sz, e1, maxk);
            g.undo_mutate();

            // Drop comp1
            g.begin_mutate();
            for (int v : comp1) g.mutate_drop(v);
            Poly p2 = comp_solve(g, nv - c1sz, edges - e1, maxk);
            g.undo_mutate();

            return poly_convolve(p1, p2, maxk);
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
    Poly p_inc = comp_solve(g, nv1, edges1, maxk - 1);
    g.undo_mutate();

    // Exclude: drop best only
    g.begin_mutate();
    g.mutate_drop(best);
    Poly p_exc = comp_solve(g, nv - 1, edges - best_deg, maxk);
    g.undo_mutate();

    for (int j = 0; j <= maxk; j++) {
        result[j] = p_exc[j];
        if (j > 0) result[j] += p_inc[j-1];
    }
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

        // Per-thread CompGraph: allocated once, reused across calls
        static thread_local CompGraph g;
        g.build(sg);
        int poly_k = (need_total <= MAX_K) ? need_total : MAX_K;
        Poly p = comp_solve(g, g.nv, g.edges, poly_k);

        count_t result = 0;
        for (int j = 0; j <= num_pivots && j <= need_total; j++) {
            int from_sg = need_total - j;
            if (from_sg >= 0 && from_sg <= poly_k)
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
    int64_t max_edges = (int64_t)nv * (nv - 1);
    int64_t comp_edges = (max_edges - sg->nedges_) / 2;
    int avg_comp_deg = (nv > 0) ? (int)(comp_edges * 2 / nv) : 0;

    // Hybrid: bitmask for nv≤512 (8 words, fits cache, vectorized AND/popcount)
    // CompGraph (adj_list + MoveToTail) for nv>512 (no vertex limit, O(active_deg) iteration)
    if (nv <= 512)
        return CountImpl<8, 512>::run(sg, max_k, clique_size, num_pivots);
    else
        return CompCountImpl::run(sg, max_k, clique_size, num_pivots);
}

} // namespace complement

#endif // COMPLEMENT_COUNT_H
