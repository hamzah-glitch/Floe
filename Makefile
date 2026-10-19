FLEX ?= flex
BISON ?= bison
CXX ?= c++
CXXFLAGS ?= -std=c++17 -Wall -Wextra -pedantic

BUILD_DIR := build
TARGET := $(BUILD_DIR)/floe
PARSER_CPP := $(BUILD_DIR)/parser.cpp
PARSER_HPP := $(BUILD_DIR)/parser.hpp
LEXER_CPP := $(BUILD_DIR)/lexer.cpp

.PHONY: all clean check-tools test test-lexer test-parser

all: check-tools $(TARGET)

check-tools:
	@command -v $(FLEX) >/dev/null || { echo "error: Flex ($(FLEX)) is required" >&2; exit 1; }
	@command -v $(BISON) >/dev/null || { echo "error: Bison ($(BISON)) is required" >&2; exit 1; }
	@command -v $(CXX) >/dev/null || { echo "error: C++ compiler ($(CXX)) is required" >&2; exit 1; }

$(BUILD_DIR):
	mkdir -p $@

$(PARSER_CPP): src/parser.y | $(BUILD_DIR)
	$(BISON) -Wall -d -o $@ $<

$(LEXER_CPP): src/lexer.l $(PARSER_CPP)
	$(FLEX) -o $@ $<

$(TARGET): $(PARSER_CPP) $(LEXER_CPP) src/main.cpp src/ast.cpp src/ast.h src/lexer.h | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) -Isrc -I$(BUILD_DIR) $(PARSER_CPP) $(LEXER_CPP) src/main.cpp src/ast.cpp -o $@

# tests/*.floe        valid programs: must lex and parse cleanly
# tests/lexer/*.floe  lexer-only inputs: must lex cleanly
# tests/errors/lexer_*.floe and parser_*.floe: must be rejected
test: test-lexer test-parser

test-lexer: all
	@status=0; 	for f in tests/*.floe tests/lexer/*.floe; do 		if ./$(TARGET) --tokens $$f >/dev/null; then echo "PASS  lex    $$f"; 		else echo "FAIL  lex    $$f (unexpected lexical error)"; status=1; fi; 	done; 	for f in tests/errors/lexer_*.floe; do 		if ./$(TARGET) --tokens $$f >/dev/null 2>&1; then echo "FAIL  lex    $$f (expected lexical errors)"; status=1; 		else echo "PASS  lex    $$f (errors detected)"; fi; 	done; 	exit $$status

test-parser: all
	@status=0; 	for f in tests/*.floe; do 		if ./$(TARGET) $$f >/dev/null; then echo "PASS  parse  $$f"; 		else echo "FAIL  parse  $$f (unexpected syntax error)"; status=1; fi; 	done; 	for f in tests/errors/parser_*.floe; do 		if ./$(TARGET) $$f >/dev/null 2>&1; then echo "FAIL  parse  $$f (expected syntax errors)"; status=1; 		else echo "PASS  parse  $$f (errors detected)"; fi; 	done; 	exit $$status

clean:
	rm -rf $(BUILD_DIR)