CROSS ?= riscv64-linux-gnu-
CC := $(CROSS)gcc
AR := $(CROSS)ar
SYSROOT ?=

CFLAGS ?= -O2 -Wall -Wextra -Werror
CPPFLAGS += -I. -I../../common
ifneq ($(SYSROOT),)
CPPFLAGS += --sysroot=$(SYSROOT) -isystem $(SYSROOT)/usr/riscv64-linux-gnu/include
LDFLAGS += --sysroot=$(SYSROOT) -L$(SYSROOT)/usr/riscv64-linux-gnu/lib
LDFLAGS += -Wl,-rpath-link=$(SYSROOT)/usr/riscv64-linux-gnu/lib
LDFLAGS += -B$(SYSROOT)/usr/riscv64-linux-gnu/lib/
endif

BUILD_DIR := build
LIB_STATIC := $(BUILD_DIR)/libgpgpu.a
LIB_SHARED := $(BUILD_DIR)/libgpgpu.so
OBJS := $(BUILD_DIR)/libgpgpu.o

.PHONY: all clean

all: $(LIB_STATIC) $(LIB_SHARED)

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -c -o $@ $<

$(LIB_STATIC): $(OBJS)
	$(AR) rcs $@ $^

$(LIB_SHARED): $(OBJS)
	$(CC) $(LDFLAGS) -shared -o $@ $^

clean:
	rm -rf $(BUILD_DIR)
