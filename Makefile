# CloverIS build system
# Produces three k-clique counting binaries and a graph converter.
#
# clover      — Cover-based pivoting (no IS)
# clover_is   — Cover-based pivoting + complement IS (the proposed algorithm)
# pivotscale  — PivotScale baseline (independent source tree)
# converter   — SNAP edge-list → .sg serialized format
#
# For graphs with very large clique counts (k >= 10 on large graphs),
# build with USE_128=1 to use 128-bit integers and avoid overflow:
#   make USE_128=1

CXX      ?= g++
CXXFLAGS  = -std=c++20 -O3 -fopenmp -march=native -Wall
BIN       = bin

ifeq ($(USE_128),1)
  CXXFLAGS += -DUSE_128
endif

CLOVER_HDRS     := $(wildcard src/clover/*.h)     $(wildcard src/clover/*.hpp)
PIVOTSCALE_HDRS := $(wildcard src/pivotscale/*.h) $(wildcard src/pivotscale/*.hpp)

all: $(BIN)/pivotscale $(BIN)/clover $(BIN)/clover_is $(BIN)/converter

$(BIN):
	mkdir -p $(BIN)

$(BIN)/pivotscale: src/pivotscale/pivotscale.cc $(PIVOTSCALE_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@

$(BIN)/clover: src/clover/clover.cc $(CLOVER_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) -DENABLE_IS=0 $< -o $@

$(BIN)/clover_is: src/clover/clover.cc $(CLOVER_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) -DENABLE_IS=1 $< -o $@

$(BIN)/converter: src/pivotscale/converter.cc $(PIVOTSCALE_HDRS) | $(BIN)
	$(CXX) $(CXXFLAGS) $< -o $@

clean:
	rm -rf $(BIN)

.PHONY: all clean
