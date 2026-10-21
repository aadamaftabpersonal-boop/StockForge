# StockForge build. Run inside the dev image: scripts/dev.sh make test
#   make                     release build        -> build/rel/stockforged
#   make SAN=address,undefined   ASan+UBSan build -> build/san-address-undefined/
#   make SAN=thread          TSan build           -> build/san-thread/
#   make test                unit tests
#   make itest               integration tests (needs the binary)
CC      ?= gcc
SAN     ?=
comma   := ,
BUILD   := build/$(if $(SAN),san-$(subst $(comma),-,$(SAN)),rel)
OPT     := $(if $(SAN),-O1 -fno-omit-frame-pointer -fsanitize=$(SAN),-O2)
CFLAGS  := -std=c11 -D_GNU_SOURCE -g $(OPT) -pthread -Isrc -Ivendor/picohttpparser \
           -Wall -Wextra -Werror -Wshadow -Wformat=2 -Wno-unused-parameter
LDFLAGS := -pthread $(if $(SAN),-fsanitize=$(SAN))
LDLIBS  :=

SRCS    := $(filter-out src/main.c,$(wildcard src/*.c))
OBJS    := $(SRCS:src/%.c=$(BUILD)/%.o)
PICO    := $(BUILD)/picohttpparser.o
BIN     := $(BUILD)/stockforged
TESTS   := $(patsubst tests/unit/%.c,$(BUILD)/test_%,$(wildcard tests/unit/test_*.c))

.PHONY: all test itest clean
all: $(BIN)

$(BIN): $(BUILD)/main.o $(OBJS) $(PICO)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: src/%.c | $(BUILD)
	$(CC) $(CFLAGS) -MMD -c -o $@ $<

# vendored code: compiled without -Werror/-Wshadow noise
$(PICO): vendor/picohttpparser/picohttpparser.c | $(BUILD)
	$(CC) -std=c11 -D_GNU_SOURCE -g $(OPT) -c -o $@ $<

$(BUILD)/test_%: tests/unit/%.c $(OBJS) $(PICO) | $(BUILD)
	$(CC) $(CFLAGS) -Itests/unit -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILD):
	mkdir -p $@

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "== $$t"; $$t; done

itest: $(BIN)
	SF_BIN=$(abspath $(BIN)) python3 -W ignore::ResourceWarning -m unittest discover -v -s tests/integration -p 'test_*.py'

clean:
	rm -rf build

-include $(wildcard $(BUILD)/*.d)
