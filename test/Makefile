# Radiotchi - host test harness for the portable, libm-free pure logic.
#
# Builds the Analysis Core (lib/analysis_core) and the pure pet growth
# (src/game_shell/pet_growth.c) with a plain host compiler - no furi / hardware.
#
#   make            # build + run all tests
#   make CC=clang   # pick a compiler
#
# No -lm needed (the code is deliberately libm-free).

CC      ?= cc
CFLAGS  ?= -std=c99 -Wall -Wextra -O2
CORE    := ../lib/analysis_core
GAME    := ../src/game_shell
BUILD   := build

.PHONY: all test clean
all: test

$(BUILD)/test_analysis_core: test_analysis_core.c $(CORE)/analysis_core.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(CORE) $^ -o $@

$(BUILD)/test_pet_growth: test_pet_growth.c $(GAME)/pet_growth.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(CORE) -I$(GAME) $^ -o $@

test: $(BUILD)/test_analysis_core $(BUILD)/test_pet_growth
	./$(BUILD)/test_analysis_core
	./$(BUILD)/test_pet_growth

clean:
	rm -rf $(BUILD)
