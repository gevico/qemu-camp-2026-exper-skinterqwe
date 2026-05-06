CROSS ?= riscv64-linux-gnu-
CC := $(CROSS)gcc
SYSROOT ?=

CFLAGS ?= -O2 -Wall -Wextra -Werror -static
CPPFLAGS += -I../libgpgpu -I../../kernels
ifneq ($(SYSROOT),)
CPPFLAGS += --sysroot=$(SYSROOT) -isystem $(SYSROOT)/usr/riscv64-linux-gnu/include
LDFLAGS += --sysroot=$(SYSROOT) -L$(SYSROOT)/usr/riscv64-linux-gnu/lib
LDFLAGS += -Wl,-rpath-link=$(SYSROOT)/usr/riscv64-linux-gnu/lib
LDFLAGS += -B$(SYSROOT)/usr/riscv64-linux-gnu/lib/
endif

BUILD_DIR := build
TARGET := $(BUILD_DIR)/gpgpu_linux_demo
LIB := ../libgpgpu/build/libgpgpu.a

.PHONY: all clean

all: $(TARGET)

$(BUILD_DIR):
	mkdir -p $@

$(LIB):
	$(MAKE) -C ../libgpgpu CROSS=$(CROSS) SYSROOT=$(SYSROOT)

$(TARGET): main.c $(LIB) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ main.c $(LIB)

clean:
	rm -rf $(BUILD_DIR)
