CXX ?= c++
CXXSTD ?= -std=c++17
INCLUDES := -Iinclude
WARNINGS := -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion \
            -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Werror
CXXFLAGS ?= $(CXXSTD) $(INCLUDES) $(WARNINGS) -O2 -g
LDFLAGS ?= -pthread

BUILD_DIR := build
HEADERS := $(wildcard include/lru/*.hpp)
TEST_DEPS := $(HEADERS) tests/test_harness.hpp tests/conformance.hpp
TEST_SRCS := $(sort $(wildcard tests/test_*.cpp))
TEST_BINS := $(patsubst tests/%.cpp,$(BUILD_DIR)/%,$(TEST_SRCS))
EXAMPLE_SRCS := $(wildcard examples/*.cpp)
EXAMPLE_BINS := $(patsubst examples/%.cpp,$(BUILD_DIR)/%,$(EXAMPLE_SRCS))
BENCH_SRC := benchmarks/bench_lru.cpp

# LeakSanitizer is Linux-only; macOS uses the `leaks` tool instead (make leaks).
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Linux)
ASAN_ENV := ASAN_OPTIONS=detect_leaks=1
else
ASAN_ENV :=
endif

.PHONY: all test examples run bench asan ubsan tsan leaks check clean help

help:
	@echo "LRUCache: header-only LRU cache with optional TTL"
	@echo
	@echo "  make test        build and run every test suite"
	@echo "  make run         interactive cache shell (also reads piped input)"
	@echo "  make examples    build and run the example programs"
	@echo "  make bench       classic vs production benchmark"
	@echo "  make asan        tests under AddressSanitizer"
	@echo "  make ubsan       tests under UndefinedBehaviorSanitizer"
	@echo "  make tsan        tests under ThreadSanitizer"
	@echo "  make leaks       leak check"
	@echo "  make check       everything CI runs"
	@echo "  make clean       remove build artefacts"

all: test examples

# ---- tests ------------------------------------------------------------------

$(BUILD_DIR)/test_%: tests/test_%.cpp $(TEST_DEPS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

test: $(TEST_BINS)
	@for binary in $(TEST_BINS); do echo "== $$binary"; ./$$binary || exit 1; echo; done

# ---- examples ---------------------------------------------------------------

$(BUILD_DIR)/%: examples/%.cpp $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)

examples: $(EXAMPLE_BINS)
	@for binary in $(EXAMPLE_BINS); do \
		case $$binary in *cache_shell) continue;; esac; \
		echo "== $$binary"; ./$$binary || exit 1; echo; \
	done

# Interactive shell. Works with a pipe too: echo "put a 1" | make run
run: $(BUILD_DIR)/cache_shell
	@./$(BUILD_DIR)/cache_shell

# ---- benchmark --------------------------------------------------------------

$(BUILD_DIR)/bench: $(BENCH_SRC) $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXSTD) $(INCLUDES) $(WARNINGS) -O2 -DNDEBUG $(BENCH_SRC) -o $@ $(LDFLAGS)

bench: $(BUILD_DIR)/bench
	@./$(BUILD_DIR)/bench

# ---- sanitizers -------------------------------------------------------------

asan: $(TEST_SRCS) $(TEST_DEPS) | $(BUILD_DIR)
	@for src in $(TEST_SRCS); do \
		out=$(BUILD_DIR)/$$(basename $$src .cpp)_asan; \
		echo "== $$out"; \
		$(CXX) $(CXXSTD) $(INCLUDES) $(WARNINGS) -O1 -g -fno-omit-frame-pointer \
			-fsanitize=address $$src -o $$out $(LDFLAGS) || exit 1; \
		$(ASAN_ENV) ./$$out || exit 1; echo; \
	done

ubsan: $(TEST_SRCS) $(TEST_DEPS) | $(BUILD_DIR)
	@for src in $(TEST_SRCS); do \
		out=$(BUILD_DIR)/$$(basename $$src .cpp)_ubsan; \
		echo "== $$out"; \
		$(CXX) $(CXXSTD) $(INCLUDES) $(WARNINGS) -O1 -g -fno-omit-frame-pointer \
			-fsanitize=undefined -fno-sanitize-recover=undefined \
			$$src -o $$out $(LDFLAGS) || exit 1; \
		./$$out || exit 1; echo; \
	done

tsan: $(TEST_SRCS) $(TEST_DEPS) | $(BUILD_DIR)
	@for src in $(TEST_SRCS); do \
		out=$(BUILD_DIR)/$$(basename $$src .cpp)_tsan; \
		echo "== $$out"; \
		$(CXX) $(CXXSTD) $(INCLUDES) $(WARNINGS) -O1 -g -fno-omit-frame-pointer \
			-fsanitize=thread $$src -o $$out $(LDFLAGS) || exit 1; \
		./$$out || exit 1; echo; \
	done

leaks: $(TEST_BINS)
ifeq ($(UNAME_S),Darwin)
	@for binary in $(TEST_BINS); do \
		echo "== $$binary"; \
		MallocStackLogging=1 leaks --atExit -- ./$$binary 2>/dev/null \
			| grep -E "leaks for|total leaked" || exit 1; \
	done
else
	@$(MAKE) asan
endif

check: test examples asan ubsan tsan leaks

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
