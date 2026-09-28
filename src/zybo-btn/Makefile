# SPDX-License-Identifier: GPL-2.0
ifneq ($(KERNELRELEASE),)

obj-m := zybo_btn.o

else

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

.PHONY: all clean

endif
