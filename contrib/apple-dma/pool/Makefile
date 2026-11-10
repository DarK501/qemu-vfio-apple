# Out-of-tree build against the running (or specified) kernel:
#   make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
KERNELDIR ?= /lib/modules/$(shell uname -r)/build

ccflags-y += -I$(src)
obj-m += apple_dma_pool.o

all:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(CURDIR) clean

.PHONY: all clean
