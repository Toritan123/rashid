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

test: $(BIN) tests/hello.x86 tests/arith.x86 tests/fault.x86
	@echo "=== hello ==="   && ./$(BIN) tests/hello.x86
	@echo "=== arith ==="   && ./$(BIN) tests/arith.x86; \
	  echo "(expected exit status 55)"
	@echo "=== fault containment ==="; \
	  for m in null w f; do \
	    printf '  %-5s ' $$m; \
	    ./$(BIN) tests/fault.x86 $$m 2>&1 | grep -m1 '^fault:' || echo "NO FAULT REPORTED"; \
	  done

clean:
	rm -f $(BIN) tests/*.x86
