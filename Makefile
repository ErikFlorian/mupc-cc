CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
CPPFLAGS ?= -Iinclude
BUILD ?= build
CORE := src/parser.cpp src/preprocessor.cpp src/assembler.cpp src/compiler.cpp
CORE_OBJ := $(patsubst src/%.cpp,$(BUILD)/%.o,$(CORE))

.PHONY: all test clean install test-upstream test-assembler test-backend probe-upstream-address
all: $(BUILD)/mupc-cc

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: src/%.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/test_%.o: tests/%.cpp | $(BUILD)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/mupc-cc: $(CORE_OBJ) $(BUILD)/main.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/mupc-tests: $(CORE_OBJ) $(BUILD)/test_test.o
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(BUILD)/mupc-tests test-assembler test-backend
	$(BUILD)/mupc-tests

$(BUILD)/mupc-assembler-tests: $(BUILD)/assembler.o $(BUILD)/test_assembler_test.o
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/mupc-backend-tests: $(CORE_OBJ) $(BUILD)/test_backend_review.o
	$(CXX) $(CXXFLAGS) $^ -o $@

test-backend: $(BUILD)/mupc-backend-tests
	$(BUILD)/mupc-backend-tests

test-assembler: $(BUILD)/mupc-assembler-tests
	$(BUILD)/mupc-assembler-tests

PREFIX ?= /usr/local
install: $(BUILD)/mupc-cc
	install -d "$(DESTDIR)$(PREFIX)/bin"
	install -m 755 $(BUILD)/mupc-cc "$(DESTDIR)$(PREFIX)/bin/mupc-cc"

test-upstream: $(BUILD)/mupc-tests
	MUPC_TEST_IMAGE_DIR="$(BUILD)/upstream-suite" $(BUILD)/mupc-tests
	./scripts/test-upstream.sh --suite "$(BUILD)/upstream-suite"

probe-upstream-address:
	./scripts/probe-upstream-signed-address.sh

clean:
	rm -rf $(BUILD)

-include $(wildcard $(BUILD)/*.d)
