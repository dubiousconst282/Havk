#include <cstdio>
#include <Havk/Havk.h>
#include "Shaders/HelloCompute.h"

int main(int argc, const char** args) {
    auto device = havk::CreateContext({ .EnableDebugExtensions = true });

    // Allocate system RAM with caching (ideal for readbacks)
    // Discrete GPUs will read this data through PCIe, so it's best suitable for one-time sequential access.
    // Host-visible buffers are guaranteed to be host-coherent by default, so flush/invalidate calls are not necessary.
    auto buffer = device->CreateBuffer(64 * sizeof(float), havk::BufferFlags::HostMem_Cached);
    auto span = buffer->Slice<float>();

    for (int i = 0; i < span.size(); i++) {
        span[i] = i;
    }

    auto cmds = device->CreateCommandList();
    cmds->Dispatch<CS_ComputeHello>({ span.size(), 1, 1 }, { .data = span });

    // Ensure GPU caches are flushed and data is made visible to CPU.
    // A matching HOST -> SHADER barrier is not necessary, because Submit() guarantees visibility of prior host writes.
    // See https://docs.vulkan.org/spec/latest/chapters/synchronization.html#synchronization-submission-host-writes
    // `BarrierDesc{}` defaults to full stall/flush, so only need to refine what is necessary.
    cmds->Barrier({
        .SrcStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        .DstStages = VK_PIPELINE_STAGE_HOST_BIT,
        .DstAccess = VK_ACCESS_HOST_READ_BIT,
    });
    cmds->Submit().Wait();

    for (int i = 0; i < span.size(); i++) {
        if (i != 0 && i % 8 == 0) printf("\n");
        printf("%6g", span[i].value);
    }
    return 0;
}