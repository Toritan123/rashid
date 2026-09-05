CC      ?= clang
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c11 -arch arm64
SRC      = src/as.c src/macho.c src/cpu.c src/main.c
BIN      = rashid

.PHONY: all clean test

all: $(BIN)

$(BIN): $(SRC) src/as.h src/macho.h src/cpu.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

# Freestanding x86_64 guests. Linked against libSystem so the result is PIE -
# ld64 forces non-PIE with -static, and a non-PIE image wants 0x100000000,
# which rashid's own arm64 image already occupies. Nothing is actually
# imported: these guests reach the kernel through the syscall instruction.
tests/%.x86: tests/%.c
	clang -arch x86_64 -nostdlib -lSystem -Wl,-e,_start -O1 -o $@ $<

GUESTS = hello arith tls ripimm sse vm fault

test: $(BIN) $(GUESTS:%=tests/%.x86)
	@./tests/run.sh

clean:
	rm -f $(BIN) tests/*.x86
	rm -rf *.dSYM
