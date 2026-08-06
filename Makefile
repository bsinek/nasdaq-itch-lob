CXX      := clang++
CXXFLAGS := -std=c++20 -O3 -march=native -Wall -Wextra
LDLIBS   := -lz
B        := build

HDRS := $(wildcard src/*.hpp)

all: $(B)/itch-parse $(B)/itch-replay

$(B)/itch-parse: src/itch_parse.cpp src/reader.cpp $(HDRS) | $(B)
	$(CXX) $(CXXFLAGS) src/itch_parse.cpp src/reader.cpp $(LDLIBS) -o $@

# Trusted-input benchmark build: hot-path safety checks compiled out.
# bench/latency comparison only — never use for validation or export.
$(B)/itch-parse-unchecked: src/itch_parse.cpp src/reader.cpp $(HDRS) | $(B)
	$(CXX) $(CXXFLAGS) -DITCH_UNCHECKED src/itch_parse.cpp src/reader.cpp $(LDLIBS) -o $@

$(B)/itch-replay: src/itch_replay.cpp src/reader.cpp $(HDRS) | $(B)
	$(CXX) $(CXXFLAGS) src/itch_replay.cpp src/reader.cpp $(LDLIBS) -o $@

$(B)/book-test: tests/book_test.cpp $(HDRS) | $(B)
	$(CXX) $(CXXFLAGS) -Isrc tests/book_test.cpp -o $@

test: $(B)/book-test
	$(B)/book-test

$(B):
	mkdir -p $(B)

clean:
	rm -rf $(B)

.PHONY: all test clean
