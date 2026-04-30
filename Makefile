# Parallel Aho-Corasick research laboratory
# -------------------------------------------------------------------
# Build modes:
#   make            - release build (-O3 -march=native)
#   make debug      - debug build (-O0 -g, asserts on)
#   make asan       - AddressSanitizer + UBSan
#   make tsan       - ThreadSanitizer (verifies parallel correctness)
#   make test       - run correctness tests
#   make bench      - run the benchmark suite
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
DEFS    := -D_POSIX_C_SOURCE=200809L -D_GNU_SOURCE
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

# Pick up every searcher module automatically.
CORE_SRCS := $(wildcard src/*.c)
SRCH_SRCS := $(wildcard src/searchers/*.c)
ALL_SRCS  := $(CORE_SRCS) $(SRCH_SRCS)

# Separate main vs library objects.
LIB_SRCS  := $(filter-out src/main.c, $(ALL_SRCS))
LIB_OBJS  := $(LIB_SRCS:%.c=$(BUILD)/%.o)
MAIN_OBJ  := $(BUILD)/src/main.o
TEST_OBJ  := $(BUILD)/tests/test_correctness.o

.PHONY: all debug asan tsan test bench clean dirs

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

test: $(TESTBIN)
	./$(TESTBIN)

bench: $(BIN)
	@./scripts/run_benchmarks.sh

clean:
	rm -rf $(BUILD)

-include $(LIB_OBJS:.o=.d) $(MAIN_OBJ:.o=.d) $(TEST_OBJ:.o=.d)
