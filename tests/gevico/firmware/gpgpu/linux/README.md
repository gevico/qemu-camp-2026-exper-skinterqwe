# GPGPU Linux Demo

This directory contains the Linux version of the GPGPU software stack:

- `gpgpu_kmod/`: PCI kernel module plus `/dev/gpgpu` chardev
- `libgpgpu/`: user-space runtime using `mmap` for VRAM and `ioctl` for control
- `demo/`: Linux demo that reuses the same kernel machine code as bare metal

Build user-space artifacts:

```sh
make user
```

If the host cross toolchain misses RISC-V libc headers, build with a local
sysroot extracted from the Ubuntu cross packages:

```sh
apt-get download libc6-riscv64-cross libc6-dev-riscv64-cross linux-libc-dev-riscv64-cross
mkdir -p sysroot
dpkg-deb -x libc6-riscv64-cross_*_all.deb sysroot
dpkg-deb -x libc6-dev-riscv64-cross_*_all.deb sysroot
dpkg-deb -x linux-libc-dev-riscv64-cross_*_all.deb sysroot
make user SYSROOT=$PWD/sysroot
```

Build the kernel module with a matching RISC-V kernel tree:

```sh
make kmod KERNEL_SRC=/path/to/linux-build-or-source CROSS=riscv64-linux-gnu-
```

Pack a rootfs from a prepared `rootfs/` directory:

```sh
make rootfs KERNEL_SRC=/path/to/linux-build-or-source
```

Run QEMU:

```sh
make run ROOTFS_CPIO=build/gpgpu_linux.cpio.gz LINUX_IMAGE=../Image
```
