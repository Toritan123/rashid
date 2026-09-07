CC      ?= clang
CFLAGS  ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -std=c11 -arch arm64
# -lobjc: rashid registers the guest image's selectors with the native runtime.
LDFLAGS ?= -lobjc
SRC      = src/as.c src/macho.c src/fixups.c src/thunk.c src/objc.c src/callback.c src/cpu.c src/main.c src/callgate.S src/trampoline.S
BIN      = rashid

.PHONY: all clean test

all: $(BIN)

$(BIN): $(SRC) src/as.h src/macho.h src/cpu.h src/stubs.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)

# Freestanding x86_64 guests. Linked against libSystem so the result is PIE -
# ld64 forces non-PIE with -static, and a non-PIE image wants 0x100000000,
# which rashid's own arm64 image already occupies. Nothing is actually
# imported: these guests reach the kernel through the syscall instruction.
tests/%.x86: tests/%.c
	clang -arch x86_64 -nostdlib -lSystem -Wl,-e,_start -O1 -o $@ $<

GUESTS = hello arith tls ripimm sse vm fault
EXTRA  = tests/import.x86 tests/native.x86 tests/varargs.x86 tests/objc.x86 tests/callback.x86

test: $(BIN) $(GUESTS:%=tests/%.x86) $(EXTRA)
	@./tests/run.sh

clean:
	rm -f $(BIN) tests/*.x86
	rm -rf *.dSYM

# Normally linked, so it carries chained fixups and real imports.
tests/import.x86: tests/import.c
	clang -arch x86_64 -O1 -o $@ $<

tests/native.x86: tests/native.c
	clang -arch x86_64 -O1 -o $@ $<

tests/varargs.x86: tests/varargs.c
	clang -arch x86_64 -O1 -o $@ $<

tests/objc.x86: tests/objc.m
	clang -arch x86_64 -O1 -fobjc-arc -framework Foundation -o $@ $<

tests/callback.x86: tests/callback.c
	clang -arch x86_64 -O1 -o $@ $<
