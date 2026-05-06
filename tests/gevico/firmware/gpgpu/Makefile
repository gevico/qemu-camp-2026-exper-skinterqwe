CROSS    ?= riscv64-unknown-elf-
CC       := $(CROSS)gcc
OBJCOPY  := $(CROSS)objcopy
LDFLAGS  := -nostdlib -static -mcmodel=medany
CFLAGS   := -O2 -g -march=rv64g -mabi=lp64d -mcmodel=medany -nostdlib -ffreestanding -Wall -Werror

# Use realpath to resolve the CRT directory
CRT_DIR  := $(realpath ../../tcg/riscv64/crt)

SRCS := demo/main.c \
        runtime/libgpgpu.c \
        driver/gpgpu_drv.c \
        driver/pci.c

CRT_OBJS := build/crt_crt.o \
            build/crt_console.o \
            build/crt_memory.o

OBJS := $(patsubst %.c,build/%.o,$(SRCS)) $(CRT_OBJS)

LDSCRIPT := semihost.ld
TARGET   := build/gpgpu_demo

.PHONY: all clean run

all: $(TARGET)

$(TARGET): $(OBJS) $(LDSCRIPT)
	$(CC) $(LDFLAGS) -T $(LDSCRIPT) -o $@ $(OBJS)

build/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I. -c -o $@ $<

build/crt_crt.o: $(CRT_DIR)/crt.S
	@mkdir -p build
	$(CC) $(CFLAGS) -c -o $@ $<

build/crt_console.o: $(CRT_DIR)/console.c
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(CRT_DIR) -c -o $@ $<

build/crt_memory.o: $(CRT_DIR)/memory.c
	@mkdir -p build
	$(CC) $(CFLAGS) -I$(CRT_DIR) -c -o $@ $<

run: $(TARGET)
	qemu-system-riscv64 -M g233 -m 2G -display none \
		-semihosting \
		-device loader,file=$(TARGET) \
		-device gpgpu

clean:
	rm -rf build/
