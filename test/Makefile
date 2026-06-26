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

.PHONY: all test clean sub_analyze
all: test

# Offline `.sub` decode analyzer (a dev tool, not a test): `make -C test sub_analyze` then run
# `test/build/sub_analyze <capture.sub>` to see the full pure-core verdict + per-slicer diagnostics.
sub_analyze: $(BUILD)/sub_analyze
$(BUILD)/sub_analyze: ../tools/sub_analyze.c $(CORE)/analysis_core.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(CORE) $^ -o $@

$(BUILD)/test_analysis_core: test_analysis_core.c $(CORE)/analysis_core.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(CORE) $^ -o $@

$(BUILD)/test_pet_growth: test_pet_growth.c $(GAME)/pet_growth.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(CORE) -I$(GAME) $^ -o $@

$(BUILD)/test_pet_mood: test_pet_mood.c $(GAME)/pet_mood.c $(GAME)/pet_growth.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(CORE) -I$(GAME) $^ -o $@

$(BUILD)/test_pet_quests: test_pet_quests.c $(GAME)/pet_quests.c $(GAME)/pet_growth.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(CORE) -I$(GAME) $^ -o $@

$(BUILD)/test_labels: test_labels.c $(GAME)/radiotchi_labels.c
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -I$(CORE) -I$(GAME) $^ -o $@

test: $(BUILD)/test_analysis_core $(BUILD)/test_pet_growth $(BUILD)/test_pet_mood $(BUILD)/test_pet_quests $(BUILD)/test_labels
	./$(BUILD)/test_analysis_core
	./$(BUILD)/test_pet_growth
	./$(BUILD)/test_pet_mood
	./$(BUILD)/test_pet_quests
	./$(BUILD)/test_labels

clean:
	rm -rf $(BUILD)
