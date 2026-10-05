# SPDX-License-Identifier: GPL-2.0-only
obj-m += fte3600.o
KDIR ?= /lib/modules/$(shell uname -r)/build
HOSTCC ?= cc

.PHONY: all clean check

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	$(RM) fte3600-policy-test

check: fte3600-policy-test
	./fte3600-policy-test

fte3600-policy-test: fte3600-policy-test.c fte3600-policy.h
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -O2 -o $@ $<
