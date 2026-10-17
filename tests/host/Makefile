# Host-side unit tests — plain C, no Zephyr, no NCS. Separate from the firmware
# build on purpose: they need only a host compiler and run in under a second, so
# they can gate every push without the SDK.
#
#   make -C tests/host          build and run everything
#   make -C tests/host clean
#
# Units are compiled against stubs/ (a hand-written sliver of the Zephyr API),
# with sanitizers on: a signed overflow in the fixed-point math is a test
# failure here, not undefined behaviour that happens to work on the target.

CC      ?= cc
CFLAGS  ?= -O1 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function \
	   -fsanitize=address,undefined -fno-sanitize-recover=all \
	   -Istubs -DGOLDEN_DIR=\"$(CURDIR)/golden\"
LDLIBS  += -lm

TESTS   := test_tempcomp test_fsm
OUT     := build

.PHONY: all run clean
all: run

$(OUT)/%: %.c check.h $(wildcard stubs/zephyr/*.h stubs/zephyr/*/*.h) | $(OUT)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

$(OUT)/test_tempcomp: ../../src/features/measurement/tempcomp.c golden/*.csv
$(OUT)/test_fsm: ../../src/core/fsm.c

$(OUT):
	mkdir -p $@

run: $(addprefix $(OUT)/,$(TESTS))
	@set -e; for t in $^; do echo "== $$t"; ./$$t; done

clean:
	rm -rf $(OUT)
