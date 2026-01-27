[numthreads(64)]
void CS_ComputeHello(uniform float* data, uint tid: SV_DispatchThreadID) {
    data[tid] *= data[tid];

    if (tid == 0) {
        printf("Hello from compute!\n"); // shader debug printf
    }
}
