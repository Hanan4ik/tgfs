CC ?= cc
CPPFLAGS += -Iinclude -Ithird_party/cjson -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += -lcrypto -lcurl -lm
SOURCES = src/main.c src/core.c src/json.c src/bot_api.c src/probe.c src/scan.c src/init.c third_party/cjson/cJSON.c
HEADERS = include/tgfs/tgfs.h include/tgfs/bot_api.h src/internal.h include/tgfs/scan.h third_party/cjson/cJSON.h

.PHONY: all test sanitize clean
all: build/tgfs

build/tgfs: $(SOURCES) $(HEADERS)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

build/tgfs-test: $(SOURCES) $(HEADERS)
	mkdir -p build
	$(CC) $(CPPFLAGS) -DTGFS_TESTING $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

test: build/tgfs build/tgfs-test
	python3 tests/integration.py ./build/tgfs
	python3 tests/transport.py ./build/tgfs-test
	python3 tests/scan.py ./build/tgfs-test
	python3 tests/init.py ./build/tgfs-test

sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'

clean:
	rm -rf build
