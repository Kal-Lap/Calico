#!/usr/bin/env python3
"""
Summarize reproduce.sh results.

Shows coverage (which entries are present) and count agreement
across binaries. Handles the schema difference between
paper_results.csv (4 cols) and results.csv (5 cols).

Usage:
  python3 compare.py results.csv paper_results.csv
"""

import csv
import sys
from collections import defaultdict


def load(path, key_cols=("graph", "binary", "k")):
    data = {}
    with open(path) as f:
        for row in csv.DictReader(f):
            try:
                key = tuple(row[c] for c in key_cols)
                data[key] = row
            except KeyError:
                continue
    return data


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)

    results = load(sys.argv[1])
    paper = load(sys.argv[2])

    # Coverage
    total = len(paper)
    present = 0
    for key in sorted(paper.keys()):
        if key in results:
            present += 1
        else:
            print(f"  missing: {key[0]:20s} {key[1]:12s} k={key[2]}")
    print(f"Coverage: {present}/{total} paper entries in results")

    # Count agreement across binaries
    by_gk = defaultdict(dict)
    for (g, b, k), row in results.items():
        if "count" in row and row["count"]:
            by_gk[(g, k)][b] = row["count"]

    if by_gk:
        mismatches = 0
        for (g, k), bins in sorted(by_gk.items()):
            if len(set(bins.values())) > 1:
                mismatches += 1
                print(f"  count mismatch: {g} k={k} — {dict(bins)}")
        agreed = sum(
            1
            for bins in by_gk.values()
            if len(bins) >= 2 and len(set(bins.values())) == 1
        )
        print(f"Counts: {agreed} (graph,k) pairs agree across binaries")
        if mismatches:
            print(
                f"Counts: {mismatches} mismatches (may need USE_128=1 for large counts)"
            )
    else:
        print("Counts: no count column in results")


if __name__ == "__main__":
    main()
