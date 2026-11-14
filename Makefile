CC ?= cc
CPPFLAGS += -Iinclude -Ithird_party/cjson -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += -lcrypto -lm
SOURCES = src/main.c src/core.c src/json.c third_party/cjson/cJSON.c

.PHONY: all test sanitize clean
all: build/tgfs

build/tgfs: $(SOURCES) include/tgfs/tgfs.h src/internal.h third_party/cjson/cJSON.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

test: build/tgfs
	python3 tests/integration.py ./build/tgfs

sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'

clean:
	rm -rf build
