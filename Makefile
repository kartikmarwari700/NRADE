# NRADE build file (GCC + GNU Make)
CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -O2 -g -pthread -Iinclude \
          -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
LDFLAGS = -pthread

CORE = src/topology.c src/banker.c src/sync.c src/detector.c \
       src/recovery.c src/simulator.c src/logger.c
HDR  = include/nrade.h

all: nrade

nrade: $(CORE) src/main.c $(HDR)
	$(CC) $(CFLAGS) $(CORE) src/main.c -o $@ $(LDFLAGS)

test_nrade: $(CORE) tests/test_nrade.c $(HDR)
	$(CC) $(CFLAGS) $(CORE) tests/test_nrade.c -o $@ $(LDFLAGS)

# runs TC1 - TC8 and the extra checks
test: test_nrade
	./test_nrade

run: nrade
	./nrade

demo: nrade
	sh tests/demo.sh

# Linux only: needs "sudo apt install valgrind"
helgrind: test_nrade
	valgrind --tool=helgrind --error-exitcode=1 ./test_nrade

memcheck: test_nrade
	valgrind --leak-check=full --error-exitcode=1 ./test_nrade

# needs "sudo apt install graphviz"
graphs:
	for f in output/*.dot; do dot -Tpng $$f -o $${f%.dot}.png; done

clean:
	rm -rf nrade test_nrade nrade.exe test_nrade.exe output

.PHONY: all test run demo helgrind memcheck graphs clean
