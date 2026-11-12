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
	rm -f test_audio_dsp test_audio_dsp_asan test_pcm_pump

SDK = ../../lib/softwinner/include
PUMP_INC = -I../../lib/log/include -I$(SDK)/system/public/include -I$(SDK)/system/public/include/utils -I$(SDK)/middleware/include -I$(SDK)/middleware/include/utils -I$(SDK)/middleware/include/media -I$(SDK)/middleware/media/include/component -I$(SDK)/middleware/media/LIBRARY/libcedarc/include

test_pcm_pump: test_pcm_pump.c $(SRC) ../../src/record/ai2aenc.c ../../src/record/ai2aenc.h
	$(CC) $(CFLAGS) $(INC) $(PUMP_INC) -DAWCHIP=AW_V5 -Wno-type-limits -ffunction-sections -fdata-sections test_pcm_pump.c $(SRC) -Wl,--gc-sections -lm -pthread -o $@

.PHONY: pump
pump: test_pcm_pump
	./test_pcm_pump
