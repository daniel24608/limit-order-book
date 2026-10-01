# Build targets for the limit order book.
#   make test    build and run the unit tests
#   make tsan    run the unit tests under ThreadSanitizer
#   make bench   build and run the benchmark
#   make clean   remove build output
#
# Override the compiler with e.g. `make test CXX=clang++`.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -Wpedantic -O2
CPPFLAGS += -Iinclude
LDLIBS   += -pthread

BUILD := build

LIB_SRCS   := src/OrderBook.cpp src/MatchingEngine.cpp
TEST_SRCS  := $(wildcard tests/*.cpp)
BENCH_SRCS := bench/Benchmark.cpp
HEADERS    := $(wildcard include/lob/*.hpp) $(wildcard tests/*.hpp)

.PHONY: all test tsan bench clean

all: $(BUILD)/tests $(BUILD)/benchmark

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/tests: $(LIB_SRCS) $(TEST_SRCS) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -g $(LIB_SRCS) $(TEST_SRCS) -o $@ $(LDLIBS)

$(BUILD)/tests_tsan: $(LIB_SRCS) $(TEST_SRCS) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) -std=c++17 -O1 -g -fsanitize=thread $(LIB_SRCS) $(TEST_SRCS) -o $@ $(LDLIBS)

$(BUILD)/benchmark: $(LIB_SRCS) $(BENCH_SRCS) $(HEADERS) | $(BUILD)
	$(CXX) $(CPPFLAGS) -std=c++17 -O3 -DNDEBUG $(LIB_SRCS) $(BENCH_SRCS) -o $@ $(LDLIBS)

test: $(BUILD)/tests
	./$(BUILD)/tests

tsan: $(BUILD)/tests_tsan
	./$(BUILD)/tests_tsan

bench: $(BUILD)/benchmark
	./$(BUILD)/benchmark

clean:
	rm -rf $(BUILD)
