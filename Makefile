CC      = gcc
CFLAGS  = -Wall -O2 -Iinclude -MMD -MP $(EXTRA_CFLAGS)
ENGINE_LIBS = -lasound -lspeexdsp -lswresample -lavutil -lpthread -lm
APP_LIBS    = -lpthread
CLANG_FORMAT ?= clang-format

ENGINE_OBJS = src/engine/main.o src/engine/io.o src/engine/dsp.o \
	src/engine/ipc.o src/engine/vol.o src/engine/resample.o src/engine/limiter.o
APP_OBJS = src/apps/demo.o src/client/client.o
ALL_OBJS = $(ENGINE_OBJS) $(APP_OBJS)

all: alsa-engine demo

alsa-engine: $(ENGINE_OBJS)
	$(CC) -o $@ $^ $(ENGINE_LIBS)

src/engine/%.o: src/engine/%.c
	$(CC) $(CFLAGS) -c $< -o $@

src/apps/%.o: src/apps/%.c
	$(CC) $(CFLAGS) -c $< -o $@

src/client/%.o: src/client/%.c
	$(CC) $(CFLAGS) -c $< -o $@

demo: $(APP_OBJS)
	$(CC) -o $@ $^ $(APP_LIBS)

-include $(ALL_OBJS:.o=.d)

format:
	@find . -path ./.git -prune -o \( -name '*.c' -o -name '*.h' \) -type f -print0 | \
		xargs -0 $(CLANG_FORMAT) -i

check-format:
	@CLANG_FORMAT='$(CLANG_FORMAT)' ./scripts/check-format.sh

setup-hooks:
	git config core.hooksPath .githooks

clean:
	rm -f alsa-engine demo \
		src/engine/*.o src/engine/*.d src/apps/*.o src/apps/*.d \
		src/client/*.o src/client/*.d
