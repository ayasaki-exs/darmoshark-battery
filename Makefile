# SPDX-License-Identifier: GPL-2.0-or-later
#
# darmoshark-battery — build the DKMS module, the host protocol test and the
# userspace diagnostic. `make test` and `make probe` need no kernel headers.
#
#   make             # kernel module (needs linux-headers)
#   make test        # protocol vectors, host only
#   make probe       # build/tools/darmoshark-probe
#   make clean

obj-m := darmoshark_battery.o

KDIR ?= /lib/modules/$(shell uname -r)/build
CC ?= cc

.PHONY: all clean test probe

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	-$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -rf build

test:
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -Werror -I. tests/protocol_test.c -o build/protocol_test
	./build/protocol_test

probe:
	@mkdir -p build
	$(CC) -std=c11 -Wall -Wextra -Werror -I. tools/darmoshark-probe.c -o build/darmoshark-probe
