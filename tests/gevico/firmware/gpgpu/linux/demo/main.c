#include <stdint.h>
#include <stdio.h>

#include "../libgpgpu/libgpgpu.h"
#include "../../kernels/kernels.h"

#define BLOCK_SIZE 32

static void test_vector_add(void)
{
    printf("=== Vector Add ===\n");
    const int n = 64;
    float a[64], b[64], c[64];
    int pass = 1;

    for (int i = 0; i < n; i++) {
        a[i] = (float)i;
        b[i] = (float)(i * 2);
    }

    uint32_t da = gpgpuMalloc(n * sizeof(float));
    uint32_t db = gpgpuMalloc(n * sizeof(float));
    uint32_t dc = gpgpuMalloc(n * sizeof(float));

    gpgpuMemcpyH2D(da, a, n * sizeof(float));
    gpgpuMemcpyH2D(db, b, n * sizeof(float));

    uint32_t args[] = { da, db, dc, (uint32_t)n, (uint32_t)BLOCK_SIZE };
    dim3 grid = { (n + BLOCK_SIZE - 1) / BLOCK_SIZE, 1, 1 };
    dim3 block = { BLOCK_SIZE, 1, 1 };

    gpgpuLaunchKernel(kernel_vector_add,
                      sizeof(kernel_vector_add) / sizeof(uint32_t),
                      grid, block, args, 5);
    gpgpuDeviceSynchronize();
    gpgpuMemcpyD2H(c, dc, n * sizeof(float));

    for (int i = 0; i < n; i++) {
        float expected = (float)(i + i * 2);
        if ((int)c[i] != (int)expected) {
            printf("  FAIL at %d: got %d, expected %d\n",
                   i, (int)c[i], (int)expected);
            pass = 0;
        }
    }
    if (pass)
        printf("  PASS (N=%d)\n", n);
}

static void test_relu(void)
{
    printf("=== ReLU ===\n");
    const int n = 32;
    float input[32], output[32];
    int pass = 1;

    for (int i = 0; i < n; i++)
        input[i] = (float)(i - 16);

    uint32_t din = gpgpuMalloc(n * sizeof(float));
    uint32_t dout = gpgpuMalloc(n * sizeof(float));

    gpgpuMemcpyH2D(din, input, n * sizeof(float));

    uint32_t args[] = { din, dout, (uint32_t)n, (uint32_t)BLOCK_SIZE };
    dim3 grid = { (n + BLOCK_SIZE - 1) / BLOCK_SIZE, 1, 1 };
    dim3 block = { BLOCK_SIZE, 1, 1 };

    gpgpuLaunchKernel(kernel_relu, sizeof(kernel_relu) / sizeof(uint32_t),
                      grid, block, args, 4);
    gpgpuDeviceSynchronize();
    gpgpuMemcpyD2H(output, dout, n * sizeof(float));

    for (int i = 0; i < n; i++) {
        float expected = input[i] > 0 ? input[i] : 0;
        if ((int)output[i] != (int)expected) {
            printf("  FAIL at %d: got %d, expected %d\n",
                   i, (int)output[i], (int)expected);
            pass = 0;
        }
    }
    if (pass)
        printf("  PASS (N=%d)\n", n);
}

static void test_matmul(void)
{
    printf("=== MatMul ===\n");
    const int m = 4, n = 4, k = 4;
    float a[16], b[16], c[16];
    int pass = 1;

    for (int i = 0; i < m; i++) {
        for (int j = 0; j < k; j++)
            a[i * k + j] = (float)(i == j ? 1 : 0);
    }
    for (int i = 0; i < k; i++) {
        for (int j = 0; j < n; j++)
            b[i * n + j] = (float)(i * n + j);
    }

    uint32_t da = gpgpuMalloc(m * k * sizeof(float));
    uint32_t db = gpgpuMalloc(k * n * sizeof(float));
    uint32_t dc = gpgpuMalloc(m * n * sizeof(float));

    gpgpuMemcpyH2D(da, a, m * k * sizeof(float));
    gpgpuMemcpyH2D(db, b, k * n * sizeof(float));

    uint32_t args[] = { da, db, dc, (uint32_t)n, (uint32_t)k, (uint32_t)n };
    dim3 grid = { m, 1, 1 };
    dim3 block = { n, 1, 1 };

    gpgpuLaunchKernel(kernel_matmul, sizeof(kernel_matmul) / sizeof(uint32_t),
                      grid, block, args, 6);
    gpgpuDeviceSynchronize();
    gpgpuMemcpyD2H(c, dc, m * n * sizeof(float));

    for (int i = 0; i < m; i++) {
        for (int j = 0; j < n; j++) {
            float expected = b[i * n + j];
            if (c[i * n + j] != expected) {
                printf("  FAIL at [%d][%d]: got %d, expected %d\n",
                       i, j, (int)c[i * n + j], (int)expected);
                pass = 0;
            }
        }
    }
    if (pass)
        printf("  PASS (%dx%d @ %dx%d)\n", m, n, k, n);
}

int main(void)
{
    gpgpuDeviceInfo info;

    printf("GPGPU Linux Demo\n");
    if (gpgpuInit() != 0) {
        perror("gpgpuInit");
        return 1;
    }

    gpgpuGetDeviceInfo(&info);
    printf("Device: %u CUs, %u warps/CU, %u threads/warp, %lluMB VRAM\n",
           info.num_cus, info.warps_per_cu, info.warp_size,
           (unsigned long long)(info.vram_size / (1024 * 1024)));

    test_vector_add();
    test_relu();
    test_matmul();

    gpgpuShutdown();
    printf("All demos completed.\n");
    return 0;
}
