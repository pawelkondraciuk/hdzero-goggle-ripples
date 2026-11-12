CC ?= cc
CFLAGS ?= -std=gnu11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter
SRC = ../../src/record/audio_dsp.c
INC = -I../../src/record
.DEFAULT_GOAL := all

.PHONY: all run asan clean

all: test_audio_dsp

test_audio_dsp: test_audio_dsp.c $(SRC) ../../src/record/audio_dsp.h
	$(CC) $(CFLAGS) $(INC) test_audio_dsp.c $(SRC) -lm -o $@

test_audio_dsp_asan: test_audio_dsp.c $(SRC) ../../src/record/audio_dsp.h
	$(CC) -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(INC) test_audio_dsp.c $(SRC) -lm -o $@

run: all
	./test_audio_dsp

asan: test_audio_dsp_asan
	./test_audio_dsp_asan

clean:
	rm -f test_audio_dsp test_audio_dsp_asan
