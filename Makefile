CC      ?= clang
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c11 -arch arm64
SRC      = src/as.c src/macho.c src/cpu.c src/main.c
BIN      = rashid

.PHONY: all clean test

all: $(BIN)

$(BIN): $(SRC) src/as.h src/macho.h src/cpu.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

# Freestanding x86_64 guests: no dylibs, so M0 can run them end to end.
tests/%.x86: tests/%.c
	clang -arch x86_64 -nostdlib -static -Wl,-e,_start -O1 -o $@ $<

GUESTS = hello arith tls ripimm fault

test: $(BIN) $(GUESTS:%=tests/%.x86)
	@./tests/run.sh

clean:
	rm -f $(BIN) tests/*.x86
	rm -rf *.dSYM
