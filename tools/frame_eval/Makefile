CXX     := g++
CXXFLAGS := -O2 -std=c++17 -Wall -Wextra -Wno-unused-parameter

.PHONY: all clean test

all: test_frame

test_frame: test_frame.cpp ../../app/src/main/cpp/procedural_frame.inl
	$(CXX) $(CXXFLAGS) test_frame.cpp -o test_frame

target_grid.txt: sura_border.svg render_target.py
	python3 render_target.py >/dev/null

# Default scoring run: sweep every seed bucket via stride-3600 sampling
# (one per corner-fill option) plus the first 100 contiguous seeds.
test: test_frame target_grid.txt
	./test_frame --sweep 0 100 | python3 score_seeds.py --top 12

# Full sweep of all 18 000 unique combinations (~few seconds on host).
sweep: test_frame target_grid.txt
	./test_frame --sweep 0 18000 | python3 score_seeds.py --top 20

clean:
	rm -f test_frame
