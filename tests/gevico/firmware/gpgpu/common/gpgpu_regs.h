#ifndef GPGPU_REGS_H
#define GPGPU_REGS_H

/* PCI Configuration */
#define GPGPU_VENDOR_ID         0x1234
#define GPGPU_DEVICE_ID         0x1337

/* Device Information (BAR0, read-only) */
#define GPGPU_REG_DEV_ID            0x0000
#define GPGPU_REG_DEV_VERSION       0x0004
#define GPGPU_REG_DEV_CAPS          0x0008
#define GPGPU_REG_VRAM_SIZE_LO      0x000C
#define GPGPU_REG_VRAM_SIZE_HI      0x0010

#define GPGPU_DEV_ID_VALUE          0x47505055
#define GPGPU_DEV_VERSION_VALUE     0x00010000

/* Global Control */
#define GPGPU_REG_GLOBAL_CTRL       0x0100
#define GPGPU_REG_GLOBAL_STATUS     0x0104
#define GPGPU_REG_ERROR_STATUS      0x0108

#define GPGPU_CTRL_ENABLE           (1 << 0)
#define GPGPU_CTRL_RESET            (1 << 1)

#define GPGPU_STATUS_READY          (1 << 0)
#define GPGPU_STATUS_BUSY           (1 << 1)
#define GPGPU_STATUS_ERROR          (1 << 2)

/* Interrupt Control */
#define GPGPU_REG_IRQ_ENABLE        0x0200
#define GPGPU_REG_IRQ_STATUS        0x0204
#define GPGPU_REG_IRQ_ACK           0x0208

#define GPGPU_IRQ_KERNEL_DONE       (1 << 0)
#define GPGPU_IRQ_DMA_DONE          (1 << 1)
#define GPGPU_IRQ_ERROR             (1 << 2)

/* Kernel Dispatch */
#define GPGPU_REG_KERNEL_ADDR_LO    0x0300
#define GPGPU_REG_KERNEL_ADDR_HI    0x0304
#define GPGPU_REG_KERNEL_ARGS_LO    0x0308
#define GPGPU_REG_KERNEL_ARGS_HI    0x030C
#define GPGPU_REG_GRID_DIM_X        0x0310
#define GPGPU_REG_GRID_DIM_Y        0x0314
#define GPGPU_REG_GRID_DIM_Z        0x0318
#define GPGPU_REG_BLOCK_DIM_X       0x031C
#define GPGPU_REG_BLOCK_DIM_Y       0x0320
#define GPGPU_REG_BLOCK_DIM_Z       0x0324
#define GPGPU_REG_SHARED_MEM_SIZE   0x0328
#define GPGPU_REG_DISPATCH          0x0330

/* DMA Engine */
#define GPGPU_REG_DMA_SRC_LO        0x0400
#define GPGPU_REG_DMA_SRC_HI        0x0404
#define GPGPU_REG_DMA_DST_LO        0x0408
#define GPGPU_REG_DMA_DST_HI        0x040C
#define GPGPU_REG_DMA_SIZE          0x0410
#define GPGPU_REG_DMA_CTRL          0x0414
#define GPGPU_REG_DMA_STATUS        0x0418

#define GPGPU_DMA_START             (1 << 0)
#define GPGPU_DMA_DIR_TO_VRAM       (0 << 1)
#define GPGPU_DMA_DIR_FROM_VRAM     (1 << 1)
#define GPGPU_DMA_IRQ_ENABLE        (1 << 2)

#define GPGPU_DMA_BUSY              (1 << 0)
#define GPGPU_DMA_COMPLETE          (1 << 1)
#define GPGPU_DMA_ERROR             (1 << 2)

/* VRAM Layout */
#define GPGPU_KERNEL_CODE_BASE      0x0000
#define GPGPU_KERNEL_CODE_MAX       0x1000
#define GPGPU_KERNEL_ARGS_BASE      0x1000
#define GPGPU_DATA_BASE             0x2000

#endif /* GPGPU_REGS_H */
