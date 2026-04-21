# CloverIS build system
# Produces the paper k-clique counting binaries, compact stats variants,
# and a graph converter.
#
# clover             — Cover-based pivoting (no IS)
# clover_is          — Cover-based pivoting + complement IS (proposed)
# pivotscale         — PivotScale baseline
# *_stats            — same algorithms with aggregate paper-figure stats
# converter          — SNAP edge-list / Matrix Market -> .sg serialized format
#
# 128-bit integer counts are enabled by default to avoid silent overflow
# at large k on the paper's graphs (e.g. webbase-2001 k=12 ≈ 10^18).
# To force 64-bit counts, build with USE_128=0.

CXX      ?= g++
CXXFLAGS  = -std=c++20 -O3 -fopenmp -march=native -Wall
BIN       = bin

USE_128 ?= 1
ifeq ($(USE_128),1)
  CXXFLAGS += -DUSE_128
endif

CLOVER_HDRS     := $(wildcard src/clover/*.h)     $(wildcard src/clover/*.hpp)
PIVOTSCALE_HDRS := $(wildcard src/pivotscale/*.h) $(wildcard src/pivotscale/*.hpp)

all: $(BIN)/pivotscale $(BIN)/clover $(BIN)/clover_is \
     $(BIN)/pivotscale_stats $(BIN)/clover_stats $(BIN)/clover_is_stats \
     $(BIN)/converter

$(BIN):
	mkdir -p $(BIN)

$(BIN)/pivotscale: src/pivotscale/pivotscale.cc $(PIVOTSCALE_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BIN)/pivotscale_stats: src/pivotscale/pivotscale.cc $(PIVOTSCALE_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) -DENABLE_STATS=1 $< -o $@

$(BIN)/clover: src/clover/clover.cc $(CLOVER_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) -DENABLE_IS=0 $< -o $@

$(BIN)/clover_is: src/clover/clover.cc $(CLOVER_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) -DENABLE_IS=1 $< -o $@

$(BIN)/clover_stats: src/clover/clover.cc $(CLOVER_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) -DENABLE_IS=0 -DENABLE_STATS=1 $< -o $@

$(BIN)/clover_is_stats: src/clover/clover.cc $(CLOVER_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) -DENABLE_IS=1 -DENABLE_STATS=1 $< -o $@

$(BIN)/converter: src/pivotscale/converter.cc $(PIVOTSCALE_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -rf $(BIN)

.PHONY: all clean
