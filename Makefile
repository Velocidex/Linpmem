MNAME = linpmem

obj-m += $(MNAME).o
linpmem-objs += src/linpmem.o src/pte_mmap.o

MDIR ?= $(shell pwd)
KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all clean modules

all: clean modules

modules:
	make -C $(KDIR) M=$(MDIR) modules

clean:
	make -C $(KDIR) M=$(MDIR) clean
