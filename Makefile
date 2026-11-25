# Olive Press Simulation - Makefile
#   make            build everything that exists (olive_press once main.c is in)
#   make tests      build the module tests
#   make test       build and run the module tests
#   make clean

CC       ?= gcc
CPPFLAGS += -Iinclude
CFLAGS   ?= -Wall -Wextra -g -O2
CFLAGS   += -fopenmp
LDLIBS   += -lm
GL_LIBS  ?= -lGL -lGLU -lglut

COMMON_SRC  := $(wildcard common/*.c)
PROCESS_SRC := $(wildcard processes/*.c)
GRAPH_SRC   := $(wildcard graphics/*.c)
HEADERS     := $(wildcard include/*.h)

obj = $(patsubst %.c,build/%.o,$(1))

# objects the module tests link against (no OpenGL needed)
TEST_DEPS := $(call obj,$(COMMON_SRC) processes/malaxation.c \
                        processes/separation.c processes/quality_control.c)

TESTS := build/test_separation_mass build/test_quality_control \
         build/test_malax_sep

ALL := $(if $(wildcard main.c),olive_press) $(TESTS)

all: $(ALL)

olive_press: $(call obj,main.c $(COMMON_SRC) $(PROCESS_SRC) $(GRAPH_SRC))
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS) $(GL_LIBS)

tests: $(TESTS)

build/test_%: tests/test_%.c $(TEST_DEPS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: %.c $(HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: tests
	./build/test_separation_mass config.txt
	./build/test_quality_control config.txt
	./build/test_malax_sep config.txt

clean:
	rm -rf build olive_press

.PHONY: all tests test clean
