CC ?= cc
CPPFLAGS := -D_POSIX_C_SOURCE=200809L -Iinclude
CFLAGS := -std=c11 -Wall -Wextra -Wpedantic -Werror -g
LDLIBS := -lm

COMMON_SOURCES := common/common.c common/config.c common/ipc.c common/timing.c
PROCESS_SOURCES := processes/reception.c processes/cleaning.c processes/placeholder.c

.PHONY: all clean test

all: olive_press

olive_press: main.c $(COMMON_SOURCES) $(PROCESS_SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

test_reception_cleaning: tests/test_reception_cleaning.c $(COMMON_SOURCES) \
		processes/reception.c processes/cleaning.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

test_ipc: tests/test_ipc.c common/ipc.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $^ -o $@ $(LDLIBS)

test: test_ipc test_reception_cleaning olive_press
	./test_ipc
	./test_reception_cleaning
	./olive_press
	sh tests/test_shutdown.sh

clean:
	$(RM) olive_press test_ipc test_reception_cleaning
