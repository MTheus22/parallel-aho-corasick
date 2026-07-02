# Parallel Aho-Corasick research laboratory
# -------------------------------------------------------------------
# Build modes:
#   make            - release build (-O3 -march=native)
#   make debug      - debug build (-O0 -g, asserts on)
#   make asan       - AddressSanitizer + UBSan
#   make tsan       - ThreadSanitizer (verifies parallel correctness)
#   make test       - run correctness tests
#   make bench      - run the canonical TCC sweep (scripts/run_i5_sweep.sh)
#   make ws-skew    - prepare and launch workstation skew run (phase H)
#   make clean
#
# To plug in a new parallel searcher, drop src/searchers/<name>.c
# defining `ac_searcher_t` and registering it via
# __attribute__((constructor)). It will be picked up automatically.

CC      ?= cc
STD     := -std=c11
WARN    := -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -Wmissing-prototypes -Wpointer-arith -Wcast-align
INC     := -Iinclude
# Embed the short git commit so every log/CSV row is self-describing.
# Evaluated at make-parse time; degrades to "unknown" outside a git tree.
GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
DEFS    := -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE -DAC_GIT_HASH=\"$(GIT_HASH)\"
LIBS    := -lpthread -lm

REL_FLAGS := -O3 -march=native -DNDEBUG
DBG_FLAGS := -O0 -g3 -fno-omit-frame-pointer
ASAN_FLAGS:= -O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer
TSAN_FLAGS:= -O1 -g3 -fsanitize=thread -fno-omit-frame-pointer

CFLAGS  ?= $(REL_FLAGS)
CFLAGS  += $(STD) $(WARN) $(INC) $(DEFS)

BUILD   := build
BIN     := $(BUILD)/aclab
TESTBIN := $(BUILD)/test_correctness
DIGESTBIN := $(BUILD)/test_digest

# Pick up every searcher module automatically.
CORE_SRCS := $(wildcard src/*.c)
SRCH_SRCS := $(wildcard src/searchers/*.c)
ALL_SRCS  := $(CORE_SRCS) $(SRCH_SRCS)

# Separate main vs library objects.
LIB_SRCS  := $(filter-out src/main.c, $(ALL_SRCS))
LIB_OBJS  := $(LIB_SRCS:%.c=$(BUILD)/%.o)
MAIN_OBJ  := $(BUILD)/src/main.o
TEST_OBJ  := $(BUILD)/tests/test_correctness.o
DIGEST_OBJ := $(BUILD)/tests/test_digest.o

.PHONY: all debug asan tsan test digest bench ws-skew ws-skew-check clean dirs

all: $(BIN)

debug:    CFLAGS := $(DBG_FLAGS) $(STD) $(WARN) $(INC) $(DEFS)
debug:    clean $(BIN)

asan:     CFLAGS := $(ASAN_FLAGS) $(STD) $(WARN) $(INC) $(DEFS)
asan:     LDFLAGS += -fsanitize=address,undefined
asan:     clean $(BIN) $(TESTBIN)

tsan:     CFLAGS := $(TSAN_FLAGS) $(STD) $(WARN) $(INC) $(DEFS)
tsan:     LDFLAGS += -fsanitize=thread
tsan:     clean $(BIN) $(TESTBIN)

$(BIN): $(LIB_OBJS) $(MAIN_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(TESTBIN): $(LIB_OBJS) $(TEST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(DIGESTBIN): $(LIB_OBJS) $(DIGEST_OBJ)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

test: $(TESTBIN)
	./$(TESTBIN)

digest: $(DIGESTBIN)
	./$(DIGESTBIN)

bench: $(BIN)
	@echo "Running canonical TCC sweep via scripts/run_i5_sweep.sh."
	@echo "Scope a subset with e.g. 'PHASES=\"A\" make bench'; the full run takes hours."
	@./scripts/run_i5_sweep.sh

ws-skew:
	@./scripts/run_workstation_skew.sh

ws-skew-check:
	@./scripts/run_workstation_skew.sh --check

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJS:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(DIGEST_OBJ:.o=.d)
