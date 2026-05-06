#ifndef GPGPU_IOCTL_H
#define GPGPU_IOCTL_H

#ifdef __KERNEL__
#include <linux/ioctl.h>
#include <linux/types.h>
#else
#include <linux/types.h>
#include <sys/ioctl.h>
#endif

#define GPGPU_IOCTL_MAGIC 'G'

struct gpgpu_info_data {
    __u32 num_cus;
    __u32 warps_per_cu;
    __u32 warp_size;
    __u32 reserved;
    __u64 vram_size;
};

struct gpgpu_memcpy_data {
    __u64 vram_offset;
    __u64 user_ptr;
    __u64 size;
};

struct gpgpu_launch_data {
    __u32 kernel_addr;
    __u32 args_addr;
    __u32 grid[3];
    __u32 block[3];
};

#define GPGPU_IOCTL_GET_INFO    _IOR(GPGPU_IOCTL_MAGIC, 1, struct gpgpu_info_data)
#define GPGPU_IOCTL_MEMCPY_H2D  _IOW(GPGPU_IOCTL_MAGIC, 2, struct gpgpu_memcpy_data)
#define GPGPU_IOCTL_MEMCPY_D2H  _IOW(GPGPU_IOCTL_MAGIC, 3, struct gpgpu_memcpy_data)
#define GPGPU_IOCTL_LAUNCH      _IOW(GPGPU_IOCTL_MAGIC, 4, struct gpgpu_launch_data)

#endif /* GPGPU_IOCTL_H */
