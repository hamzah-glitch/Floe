FLEX ?= flex
BISON ?= bison
CXX ?= g++
CC ?= cc
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pedantic
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -fPIC
LDFLAGS ?=

# LLVM is only needed for the compiler (Phase 2). Ubuntu ships llvm-config-16.
LLVM_CONFIG ?= $(shell command -v llvm-config-16 2>/dev/null || command -v llvm-config 2>/dev/null)

BUILD_DIR := build
INTERPRETER := $(BUILD_DIR)/floe
COMPILER := $(BUILD_DIR)/floec
RUNTIME_OBJ := $(BUILD_DIR)/floe_runtime.o
PARSER_CPP := $(BUILD_DIR)/parser.cpp
LEXER_CPP := $(BUILD_DIR)/lexer.cpp

HEADERS := $(wildcard src/*.h) src/runtime/floe_runtime.h
# Front end + semantic analysis, shared by both programs.
FRONT_OBJS := $(BUILD_DIR)/parser.o $(BUILD_DIR)/lexer.o $(BUILD_DIR)/ast.o \
              $(BUILD_DIR)/sema.o $(BUILD_DIR)/driver.o

.PHONY: all interpreter compiler clean check-tools check-llvm \
        test test-lexer test-parser test-sema test-interpreter test-compiler

all: interpreter compiler

interpreter: check-tools $(INTERPRETER)
compiler: check-tools check-llvm $(COMPILER) $(RUNTIME_OBJ)

check-tools:
	@command -v $(FLEX) >/dev/null || { echo "error: Flex ($(FLEX)) is required" >&2; exit 1; }
	@command -v $(BISON) >/dev/null || { echo "error: Bison ($(BISON)) is required" >&2; exit 1; }
	@command -v $(CXX) >/dev/null || { echo "error: C++ compiler ($(CXX)) is required" >&2; exit 1; }

check-llvm:
	@test -n "$(LLVM_CONFIG)" || { echo "error: llvm-config not found (install llvm-16-dev)" >&2; exit 1; }

$(BUILD_DIR):
	mkdir -p $@

# ---- generated sources ----
$(PARSER_CPP): src/parser.y | $(BUILD_DIR)
	$(BISON) -Wall -d -o $@ $<

$(LEXER_CPP): src/lexer.l $(PARSER_CPP)
	$(FLEX) -o $@ $<

# ---- objects ----
$(BUILD_DIR)/parser.o: $(PARSER_CPP) $(HEADERS)
	$(CXX) $(CXXFLAGS) -Isrc -I$(BUILD_DIR) -c $< -o $@

$(BUILD_DIR)/lexer.o: $(LEXER_CPP) $(HEADERS)
	$(CXX) $(CXXFLAGS) -Isrc -I$(BUILD_DIR) -c $< -o $@

$(BUILD_DIR)/%.o: src/%.cpp $(HEADERS) $(PARSER_CPP) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isrc -I$(BUILD_DIR) -c $< -o $@

# GCC 13 reports a false -Wmaybe-uninitialized inside std::variant at -O2.
$(BUILD_DIR)/interpreter.o: CXXFLAGS += -Wno-maybe-uninitialized

# The code generator needs the LLVM headers (as system headers: no warnings from them).
$(BUILD_DIR)/codegen.o: src/codegen.cpp $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isrc -isystem $(shell $(LLVM_CONFIG) --includedir) -c $< -o $@

$(RUNTIME_OBJ): src/runtime/floe_runtime.c src/runtime/floe_runtime.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# ---- programs ----
# Phase 1: Flex + Bison interpreter (no LLVM).
$(INTERPRETER): $(FRONT_OBJS) $(BUILD_DIR)/interpreter.o $(BUILD_DIR)/main.o $(RUNTIME_OBJ)
	$(CXX) $(LDFLAGS) $^ -o $@ -pthread -lm

# Phase 2: LLVM ahead-of-time compiler.
$(COMPILER): $(FRONT_OBJS) $(BUILD_DIR)/codegen.o $(BUILD_DIR)/floec.o
	$(CXX) $(LDFLAGS) $^ -o $@ $(shell $(LLVM_CONFIG) --ldflags) $(shell $(LLVM_CONFIG) --libs) \
	    -Wl,-rpath,$(shell $(LLVM_CONFIG) --libdir)

# ---- tests (see tests/run_tests.sh) ----
test: all
	@sh tests/run_tests.sh all

test-lexer: interpreter
	@sh tests/run_tests.sh lexer

test-parser: interpreter
	@sh tests/run_tests.sh parser

test-sema: interpreter
	@sh tests/run_tests.sh sema

test-interpreter: interpreter
	@sh tests/run_tests.sh interpreter

test-compiler: compiler
	@sh tests/run_tests.sh compiler

clean:
	rm -rf $(BUILD_DIR) tests/.out
