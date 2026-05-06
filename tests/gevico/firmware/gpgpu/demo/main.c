#include <stdint.h>
#include <stddef.h>
#include "../runtime/libgpgpu.h"
#include "../kernels/kernels.h"
#include "../../tcg/riscv64/crt/crt.h"

#define BLOCK_SIZE 32

static void test_vector_add(void)
{
    printf("=== Vector Add ===\n");
    const int N = 64;
    float A[64], B[64], C[64];

    for (int i = 0; i < N; i++) {
        A[i] = (float)i;
        B[i] = (float)(i * 2);
    }

    uint32_t dA = gpgpuMalloc(N * sizeof(float));
    uint32_t dB = gpgpuMalloc(N * sizeof(float));
    uint32_t dC = gpgpuMalloc(N * sizeof(float));

    gpgpuMemcpyH2D(dA, A, N * sizeof(float));
    gpgpuMemcpyH2D(dB, B, N * sizeof(float));

    uint32_t args[] = { dA, dB, dC, (uint32_t)N, (uint32_t)BLOCK_SIZE };
    dim3 grid = { (N + BLOCK_SIZE - 1) / BLOCK_SIZE, 1, 1 };
    dim3 block = { BLOCK_SIZE, 1, 1 };
    gpgpuLaunchKernel(kernel_vector_add,
                      sizeof(kernel_vector_add) / sizeof(uint32_t),
                      grid, block, args, 5);
    gpgpuDeviceSynchronize();

    gpgpuMemcpyD2H(C, dC, N * sizeof(float));

    int pass = 1;
    for (int i = 0; i < N; i++) {
        float expected = (float)(i + i * 2);
        if ((int)C[i] != (int)expected) {
            printf("  FAIL at %d: got %d, expected %d\n",
                   i, (int)C[i], (int)expected);
            pass = 0;
        }
    }
    if (pass) printf("  PASS (N=%d)\n", N);
}

static void test_relu(void)
{
    printf("=== ReLU ===\n");
    const int N = 32;
    float input[32], output[32];

    for (int i = 0; i < N; i++) {
        input[i] = (float)(i - 16);
    }

    uint32_t d_in  = gpgpuMalloc(N * sizeof(float));
    uint32_t d_out = gpgpuMalloc(N * sizeof(float));

    gpgpuMemcpyH2D(d_in, input, N * sizeof(float));

    uint32_t args[] = { d_in, d_out, (uint32_t)N, (uint32_t)BLOCK_SIZE };
    dim3 grid = { (N + BLOCK_SIZE - 1) / BLOCK_SIZE, 1, 1 };
    dim3 block = { BLOCK_SIZE, 1, 1 };
    gpgpuLaunchKernel(kernel_relu,
                      sizeof(kernel_relu) / sizeof(uint32_t),
                      grid, block, args, 4);
    gpgpuDeviceSynchronize();

    gpgpuMemcpyD2H(output, d_out, N * sizeof(float));

    int pass = 1;
    for (int i = 0; i < N; i++) {
        float expected = (input[i] > 0) ? input[i] : 0;
        if ((int)output[i] != (int)expected) {
            printf("  FAIL at %d: got %d, expected %d\n",
                   i, (int)output[i], (int)expected);
            pass = 0;
        }
    }
    if (pass) printf("  PASS (N=%d)\n", N);
}

static void test_matmul(void)
{
    printf("=== MatMul ===\n");
    const int M = 4, N = 4, K = 4;
    float A[16], B[16], C[16];

    for (int i = 0; i < M; i++) {
        for (int j = 0; j < K; j++) {
            A[i * K + j] = (float)(i == j ? 1 : 0);
        }
    }
    for (int i = 0; i < K; i++) {
        for (int j = 0; j < N; j++) {
            B[i * N + j] = (float)(i * N + j);
        }
    }

    uint32_t dA = gpgpuMalloc(M * K * sizeof(float));
    uint32_t dB = gpgpuMalloc(K * N * sizeof(float));
    uint32_t dC = gpgpuMalloc(M * N * sizeof(float));

    gpgpuMemcpyH2D(dA, A, M * K * sizeof(float));
    gpgpuMemcpyH2D(dB, B, K * N * sizeof(float));

    uint32_t args[] = { dA, dB, dC, (uint32_t)N, (uint32_t)K, (uint32_t)N };
    dim3 grid = { M, 1, 1 };
    dim3 block = { N, 1, 1 };
    gpgpuLaunchKernel(kernel_matmul,
                      sizeof(kernel_matmul) / sizeof(uint32_t),
                      grid, block, args, 6);
    gpgpuDeviceSynchronize();

    gpgpuMemcpyD2H(C, dC, M * N * sizeof(float));

    int pass = 1;
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < N; j++) {
            float expected = B[i * N + j];
            if (C[i * N + j] != expected) {
                printf("  FAIL at [%d][%d]: got %d, expected %d\n",
                       i, j, (int)C[i * N + j], (int)expected);
                pass = 0;
            }
        }
    }
    if (pass) printf("  PASS (%dx%d @ %dx%d)\n", M, N, K, N);
}

int main(void)
{
    printf("GPGPU Advanced Demo\n");

    if (gpgpuInit() != 0) {
        printf("ERROR: GPGPU device not found!\n");
        return 1;
    }

    gpgpuDeviceInfo info;
    gpgpuGetDeviceInfo(&info);
    printf("Device: %d CUs, %d warps/CU, %d threads/warp, %dMB VRAM\n",
           info.num_cus, info.warps_per_cu, info.warp_size,
           (int)(info.vram_size / (1024 * 1024)));

    test_vector_add();
    test_relu();
    test_matmul();

    printf("All demos completed.\n");
    return 0;
}
