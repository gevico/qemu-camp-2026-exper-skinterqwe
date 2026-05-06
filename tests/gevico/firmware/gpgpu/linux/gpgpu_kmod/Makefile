KERNEL_SRC ?= /lib/modules/$(shell uname -r)/build
ARCH ?= riscv
CROSS_COMPILE ?= riscv64-linux-gnu-

.PHONY: all clean

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	@if [ -d "$(KERNEL_SRC)" ]; then \
		$(MAKE) -C $(KERNEL_SRC) M=$(CURDIR) clean; \
	else \
		rm -f *.o *.ko *.mod *.mod.c *.order *.symvers .*.cmd; \
		rm -rf .tmp_versions; \
	fi
