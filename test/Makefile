# Host tests for the portable DSP and note code. Usage: make -C test
CC ?= cc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror -Wno-unused-parameter
SRC = test_tuner.c ../src/c/pitch.c ../src/c/tuner.c ../src/c/notes.c

.PHONY: test clean
test: test_tuner
	./test_tuner

test_tuner: $(SRC) ../src/c/*.h
	$(CC) $(CFLAGS) -D_DEFAULT_SOURCE -o $@ $(SRC) -lm

clean:
	rm -f test_tuner
