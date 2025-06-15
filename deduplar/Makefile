CC           := clang
OPENSSL_DIR  := /opt/homebrew/opt/openssl@3
CFLAGS       := -Wall -Wextra -pedantic -std=c11 -I$(OPENSSL_DIR)/include
LDFLAGS      := -L$(OPENSSL_DIR)/lib -Wl,-rpath,$(OPENSSL_DIR)/lib -lssl -lcrypto

SRC          := src/deduplar.c
BIN          := build/deduplar
SYMLINK      := /usr/local/bin/deduplar
SCAN_INTERVAL:= 20

all: $(BIN)

$(BIN): $(SRC)
	mkdir -p $(dir $@) $(HOME)/Library/Caches/deduplar
	$(CC) -o $@ $^ $(CFLAGS) $(LDFLAGS) \
		-DLOG_FILE=\"$(HOME)/Library/Caches/deduplar/deduplar.log\" \
		-DLOCK_FILE=\"$(HOME)/Library/Caches/deduplar/deduplar.lock\" \
		-DBIN_FILE=\"$(HOME)/Library/Caches/deduplar/deduplar.bin\" \
		-DSCAN_INTERVAL=$(SCAN_INTERVAL)

install: $(BIN)
	sudo install -m 755 $(BIN) $(SYMLINK)

uninstall:
	sudo rm -f $(SYMLINK)

clean:
	rm -rf build

.PHONY: all install uninstall clean
