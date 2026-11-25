# Olive press simulation - integrated team build
CC ?= cc
UNAME_S := $(shell uname -s)

CSTD := -std=gnu11
WARN := -Wall -Wextra -Wshadow -Wformat=2 -Wpedantic
OPT ?= -O2
CPPFLAGS := -D_POSIX_C_SOURCE=200809L -Iinclude -Iprocesses -Igraphics -MMD -MP
CFLAGS ?= $(OPT) -g
ALL_CFLAGS = $(CSTD) $(WARN) $(CPPFLAGS) $(CFLAGS)
LDLIBS := -lm

# Linux is the assignment target and builds FreeGLUT plus OpenMP. macOS uses
# its native GLUT/OpenGL frameworks for the same GUI; OpenMP still requires GCC.
ifeq ($(UNAME_S),Darwin)
GRAPH_SRC := graphics/graphics.c
OPENMP_FLAGS :=
BENCH_TARGET :=
WARN += -Wno-unknown-pragmas -Wno-deprecated-declarations
LDLIBS += -framework GLUT -framework OpenGL
else
GRAPH_SRC := graphics/graphics.c
OPENMP_FLAGS := -fopenmp
BENCH_TARGET := omp_bench
LDLIBS += -lglut -lGLU -lGL
endif

BUILD := build
BIN := olive_press

COMMON_SRC := $(wildcard common/*.c)
PROCESS_SRC := $(wildcard processes/*.c)
APP_SRC := src/main.c $(COMMON_SRC) $(PROCESS_SRC) $(GRAPH_SRC)
APP_OBJ := $(APP_SRC:%.c=$(BUILD)/%.o)
HEADERS := $(wildcard include/*.h processes/*.h graphics/*.h)

TEST_NAMES := test_ipc test_reception_cleaning test_config_simulation \
	test_wash_crush test_malax_sep test_quality_control test_separation_mass
TEST_BINS := $(addprefix $(BUILD)/,$(TEST_NAMES))
TEST_DEPS := $(COMMON_SRC) $(PROCESS_SRC)

.PHONY: all run headless bench tests test debug clean

all: $(BIN) $(BENCH_TARGET)

$(BIN): $(APP_OBJ)
	$(CC) $(ALL_CFLAGS) $(OPENMP_FLAGS) -o $@ $^ $(LDLIBS) $(OPENMP_FLAGS)

$(BUILD)/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(OPENMP_FLAGS) -c -o $@ $<

$(BUILD)/test_%: tests/test_%.c $(TEST_DEPS) $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) $(OPENMP_FLAGS) -o $@ $< $(TEST_DEPS) -lm $(OPENMP_FLAGS)

omp_bench: benchmark/openmp_benchmark.c
	$(CC) $(ALL_CFLAGS) -fopenmp -o $@ $< -lm -fopenmp

run: $(BIN)
	./$(BIN) config/default.cfg

headless: $(BIN)
	./$(BIN) config/default.cfg --headless </dev/null

bench:
ifeq ($(strip $(OPENMP_FLAGS)),)
	@echo "OpenMP benchmark requires GCC/OpenMP; run it on the target Linux system"
else
	$(MAKE) omp_bench
	./omp_bench
endif

tests: $(TEST_BINS)

test: $(BIN) tests
	@for test_bin in $(TEST_BINS); do $$test_bin || exit 1; done
	@sh tests/run_headless.sh ./$(BIN)

debug:
	$(MAKE) clean
	$(MAKE) OPT=-O0

clean:
	$(RM) -r $(BUILD) $(BIN) omp_bench

-include $(APP_OBJ:.o=.d)
