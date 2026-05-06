#ifndef GPGPU_KERNELS_H
#define GPGPU_KERNELS_H

#include <stdint.h>

/*
 * vector_add: C[i] = A[i] + B[i]  (float)
 * Args: A_addr, B_addr, C_addr, n, blockDim_x
 * Register allocation: x5=mhartid, x6=tid, x7=bid
 */
static const uint32_t kernel_vector_add[] = {
    0xF14022F3,  /* csrrs x5, mhartid, x0          */
    0x01F2F313,  /* andi  x6, x5, 0x1F  (tid)      */
    0x00D2D393,  /* srli  x7, x5, 13    (bid)      */
    0x000011B7,  /* lui   x3, 1          (0x1000)  */
    0x0001A503,  /* lw    x10, 0(x3)    (A_addr)   */
    0x0041A583,  /* lw    x11, 4(x3)    (B_addr)   */
    0x0081A603,  /* lw    x12, 8(x3)    (C_addr)   */
    0x00C1A683,  /* lw    x13, 12(x3)   (n)        */
    0x0101A703,  /* lw    x14, 16(x3)   (blockDim) */
    0x02E387B3,  /* mul   x15, x7, x14  (bid*bdim) */
    0x006787B3,  /* add   x15, x15, x6  (gid)      */
    0x02D7D063,  /* bge   x15, x13, +32             */
    0x00279813,  /* slli  x16, x15, 2   (off)      */
    0x010508B3,  /* add   x17, x10, x16 (&A[gid])  */
    0x0008A087,  /* flw   f1, 0(x17)    (A[gid])   */
    0x010589B3,  /* add   x19, x11, x16 (&B[gid])  */
    0x0009A107,  /* flw   f2, 0(x19)    (B[gid])   */
    0x002081D3,  /* fadd.s f3, f1, f2   (sum)      */
    0x01060B33,  /* add   x22, x12, x16 (&C[gid])  */
    0x003B2027,  /* fsw   f3, 0(x22)    (C[gid])   */
    0x00100073,  /* ebreak                          */
};

/*
 * relu: output[i] = max(0, input[i])  (float bit pattern)
 * Args: input_addr, output_addr, n, blockDim_x
 * Register allocation: x5=mhartid, x6=tid, x7=bid
 */
static const uint32_t kernel_relu[] = {
    0xF14022F3,  /* csrrs x5, mhartid, x0          */
    0x01F2F313,  /* andi  x6, x5, 0x1F  (tid)      */
    0x00D2D393,  /* srli  x7, x5, 13    (bid)      */
    0x000011B7,  /* lui   x3, 1          (0x1000)  */
    0x0001A503,  /* lw    x10, 0(x3)    (in_addr)  */
    0x0041A583,  /* lw    x11, 4(x3)    (out_addr) */
    0x0081A603,  /* lw    x12, 8(x3)    (n)        */
    0x0101A703,  /* lw    x14, 16(x3)   (blockDim) */
    0x02E387B3,  /* mul   x15, x7, x14  (bid*bdim) */
    0x006787B3,  /* add   x15, x15, x6  (gid)      */
    0x00C7DC63,  /* bge   x15, x12, +24             */
    0x00279813,  /* slli  x16, x15, 2   (off)      */
    0x010508B3,  /* add   x17, x10, x16 (&in)      */
    0x0008A903,  /* lw    x18, 0(x17)   (val)      */
    0x01058B33,  /* add   x22, x11, x16 (&out)     */
    0x00095663,  /* bge   x18, x0, +12  (positive) */
    0x000B2023,  /* sw    x0, 0(x22)    (out=0.0)  */
    0x00100073,  /* ebreak (negative path)          */
    0x012B2023,  /* sw    x18, 0(x22)   (out=val)  */
    0x00100073,  /* ebreak (positive path)          */
};

/*
 * matmul: C[row*N+col] = sum(A[row*K+k] * B[k*N+col])  (float)
 * Args: A_addr, B_addr, C_addr, N, K, blockDim_x
 * Register allocation: x5=mhartid, x6=col(tid), x7=row(bid)
 * Launch: grid=(M,1,1), block=(N,1,1)
 */
static const uint32_t kernel_matmul[] = {
    0xF14022F3,  /* csrrs x5, mhartid, x0          */
    0x01F2F313,  /* andi  x6, x5, 0x1F  (col)      */
    0x00D2D393,  /* srli  x7, x5, 13    (row)      */
    0x000011B7,  /* lui   x3, 1          (0x1000)  */
    0x0001A503,  /* lw    x10, 0(x3)    (A)        */
    0x0041A583,  /* lw    x11, 4(x3)    (B)        */
    0x0081A603,  /* lw    x12, 8(x3)    (C)        */
    0x00C1A683,  /* lw    x13, 12(x3)   (N)        */
    0x0101A703,  /* lw    x14, 16(x3)   (K)        */
    0x00000053,  /* fadd.s f0, f0, f0   (sum=0.0)  */
    0x00000813,  /* addi  x16, x0, 0    (k=0)      */
    0x02E85E63,  /* bge   x16, x14, +60 (exit)     */
    0x02E388B3,  /* mul   x17, x7, x14  (row*K)    */
    0x010888B3,  /* add   x17, x17, x16 (+k)       */
    0x00289893,  /* slli  x17, x17, 2   (*4)       */
    0x011508B3,  /* add   x17, x17, x10 (&A)       */
    0x0008A087,  /* flw   f1, 0(x17)    (A val)    */
    0x02D809B3,  /* mul   x19, x16, x13  (k*N)     */
    0x006989B3,  /* add   x19, x19, x6  (+col)     */
    0x00299993,  /* slli  x19, x19, 2   (*4)       */
    0x013589B3,  /* add   x19, x19, x11 (&B)       */
    0x0009A107,  /* flw   f2, 0(x19)    (B val)    */
    0x102081D3,  /* fmul.s f3, f1, f2   (A*B)      */
    0x00300053,  /* fadd.s f0, f0, f3   (sum+=)    */
    0x00180813,  /* addi  x16, x16, 1   (k++)      */
    0xFC0004E3,  /* beq   x0, x0, -56   (loop)     */
    0x02D388B3,  /* mul   x17, x7, x13  (row*N)    */
    0x006888B3,  /* add   x17, x17, x6  (+col)     */
    0x00289893,  /* slli  x17, x17, 2   (*4)       */
    0x011608B3,  /* add   x17, x17, x12 (&C)       */
    0x0008A027,  /* fsw   f0, 0(x17)    (store)    */
    0x00100073,  /* ebreak                          */
};

#endif /* GPGPU_KERNELS_H */
