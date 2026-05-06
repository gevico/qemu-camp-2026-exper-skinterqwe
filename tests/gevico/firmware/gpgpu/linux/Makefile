CROSS ?= riscv64-linux-gnu-
SYSROOT ?=
KERNEL_SRC ?= /tmp/linux-6.6.75-gpgpu
QEMU ?= ../../../../../build/qemu-system-riscv64
LINUX_IMAGE ?= ../../Image
ROOTFS_DIR ?= rootfs
ROOTFS_CPIO ?= build/gpgpu_linux.cpio.gz
BASE_ROOTFS ?= ../../rootfs.cpio.gz
CPIO ?= cpio
INIT_LOADER := build/init

.PHONY: all user kmod rootfs run clean

all: user

user:
	$(MAKE) -C libgpgpu CROSS=$(CROSS) SYSROOT=$(SYSROOT)
	$(MAKE) -C demo CROSS=$(CROSS) SYSROOT=$(SYSROOT)

kmod:
	$(MAKE) -C gpgpu_kmod KERNEL_SRC=$(KERNEL_SRC) \
		CROSS_COMPILE=$(CROSS) KBUILD_MODPOST_WARN=1

rootfs: user kmod $(INIT_LOADER)
	rm -rf build/rootfs-overlay
	mkdir -p build/rootfs-overlay/bin build/rootfs-overlay/lib/modules
	cp demo/build/gpgpu_linux_demo build/rootfs-overlay/bin/gpgpu_demo
	cp gpgpu_kmod/gpgpu.ko build/rootfs-overlay/lib/modules/gpgpu.ko
	cp $(INIT_LOADER) build/rootfs-overlay/init
	chmod +x build/rootfs-overlay/init build/rootfs-overlay/bin/gpgpu_demo
	gzip -cd $(BASE_ROOTFS) > build/rootfs.cpio
	cd build/rootfs-overlay && find . | $(CPIO) -o -H newc >> ../rootfs.cpio
	gzip -f build/rootfs.cpio
	mv build/rootfs.cpio.gz $(ROOTFS_CPIO)

$(INIT_LOADER): init_loader.c
	mkdir -p build
	$(CROSS)gcc --sysroot=$(SYSROOT) \
		-isystem $(SYSROOT)/usr/riscv64-linux-gnu/include \
		-static -O2 -Wall -Wextra -Werror \
		-L$(SYSROOT)/usr/riscv64-linux-gnu/lib \
		-Wl,-rpath-link=$(SYSROOT)/usr/riscv64-linux-gnu/lib \
		-B$(SYSROOT)/usr/riscv64-linux-gnu/lib/ \
		-o $@ $<

run:
	$(QEMU) -M virt -m 2G -display none -serial mon:stdio \
		-kernel $(LINUX_IMAGE) \
		-initrd $(ROOTFS_CPIO) \
		-append "console=ttyS0" \
		-device gpgpu

clean:
	$(MAKE) -C libgpgpu clean
	$(MAKE) -C demo clean
	$(MAKE) -C gpgpu_kmod clean
	rm -rf build
