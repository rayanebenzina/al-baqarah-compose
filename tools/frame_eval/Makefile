CXX        := g++
CXXFLAGS   := -O3 -std=c++17 -Wall -Wextra -Wno-unused-parameter
NUM_STYLES := 21600
JOBS       := 8

.PHONY: all clean test sweep sweep-parallel

all: test_frame

test_frame: test_frame.cpp ../../app/src/main/cpp/procedural_frame.inl
	$(CXX) $(CXXFLAGS) test_frame.cpp -o test_frame

target_grid.txt: sura_border.svg render_target.py
	python3 render_target.py >/dev/null

# Quick sanity sweep: first 100 seeds (corner=0..2 territory).
test: test_frame target_grid.txt
	./test_frame --sweep 0 100 | python3 score_seeds.py --top 12

# Full sweep over every seed in the design space, single process.
sweep: test_frame target_grid.txt
	./test_frame --sweep 0 $(NUM_STYLES) | python3 score_seeds.py --top 20

# Parallel sweep: split [0, NUM_STYLES) across $(JOBS) processes
# (16-core machine → 8 jobs ≈ 4× faster than serial). Output is
# concatenated, then scored. Each chunk gets exactly NUM_STYLES/JOBS
# seeds; the last chunk takes the remainder if it doesn't divide
# evenly.
sweep-parallel: test_frame target_grid.txt
	@chunk=$$(( $(NUM_STYLES) / $(JOBS) )); \
	rem=$$(( $(NUM_STYLES) - chunk * $(JOBS) )); \
	rm -f /tmp/frame_sweep_*.log; \
	for j in $$(seq 0 $$(( $(JOBS) - 1 ))); do \
	    start=$$(( j * chunk )); \
	    count=$$chunk; \
	    if [ $$j = $$(( $(JOBS) - 1 )) ]; then count=$$(( chunk + rem )); fi; \
	    ./test_frame --sweep $$start $$count > /tmp/frame_sweep_$$j.log & \
	done; \
	wait; \
	cat /tmp/frame_sweep_*.log | python3 score_seeds.py --top 20; \
	rm -f /tmp/frame_sweep_*.log

clean:
	rm -f test_frame
