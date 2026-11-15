CC ?= cc
SQLITE_LIBS ?= -lsqlite3
FUSE_CFLAGS ?= $(shell pkg-config --cflags fuse3)
FUSE_LIBS ?= $(shell pkg-config --libs fuse3)
CPPFLAGS += $(SQLITE_CFLAGS) $(patsubst -I%,-isystem%,$(FUSE_CFLAGS)) -D_FILE_OFFSET_BITS=64 -Iinclude -Ithird_party/cjson -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
LDLIBS += -lcrypto -lcurl $(SQLITE_LIBS) $(FUSE_LIBS) -lm
SOURCES = src/main.c src/core.c src/directory.c src/json.c src/bot_api.c src/probe.c src/scan.c src/init.c src/remote.c src/put.c src/cache.c src/mount.c src/fuse_backend.c third_party/cjson/cJSON.c
HEADERS = src/fuse_backend.h include/tgfs/remote.h src/cache.h include/tgfs/tgfs.h include/tgfs/bot_api.h src/internal.h include/tgfs/scan.h third_party/cjson/cJSON.h

.PHONY: all test sanitize clean
all: build/tgfs

build/tgfs: $(SOURCES) $(HEADERS)
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

build/tgfs-test: $(SOURCES) $(HEADERS)
	mkdir -p build
	$(CC) $(CPPFLAGS) -DTGFS_TESTING $(CFLAGS) $(SOURCES) $(LDFLAGS) $(LDLIBS) -o $@

build/fuse-driver: $(filter-out src/main.c,$(SOURCES)) $(HEADERS) tests/fuse_driver.c
	mkdir -p build
	$(CC) $(CPPFLAGS) -DTGFS_TESTING $(CFLAGS) tests/fuse_driver.c $(filter-out src/main.c,$(SOURCES)) $(LDFLAGS) $(LDLIBS) -o $@

test: build/tgfs build/tgfs-test build/fuse-driver
	python3 tests/integration.py ./build/tgfs
	python3 tests/transport.py ./build/tgfs-test
	python3 tests/scan.py ./build/tgfs-test
	python3 tests/init.py ./build/tgfs-test
	python3 tests/remote.py ./build/tgfs-test
	python3 tests/put.py ./build/tgfs-test
	python3 tests/cache.py ./build/tgfs-test
	python3 tests/fuse.py ./build/fuse-driver ./build/tgfs-test

sanitize:
	$(MAKE) clean
	$(MAKE) test CFLAGS='-O1 -g -std=c11 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined'

clean:
	rm -rf build
