import Havk.Core;
import Havk.ColorUtils;

#define HAVK_ENABLE_SHADER_DEBUG 1
#include <Havk/DebugTools.slangh>

[numthreads(64)]
void CS_DebugToolsDemo(
    uniform uint* storage,
    uniform ImageHandle2D<float4> colorBuffer,  // from last frame
    uniform TextureHandle2D<float4> normalBuffer,
    uniform TextureHandle2D<float> depthBuffer,
    uniform float3 viewOrigin,
    uniform float4x4 invProj,
    uint tid: SV_DispatchThreadID)
{
    // Widgets and shapes are only emitted if `dbg::IsEnabled` is set to true. The following helpers are available:
    //
    // - `dbg::EnableIfPicked()`: Enable if selected by pixel picker, comparing against DispatchThreadID or FragCoord.
    // - `dbg::EnableOnce()`: Enable one single invocation of current shader (arbitrary order, as determined by atomic racing).
    //
    // Multiple invocations can be enabled, but can artifact/flicker under high stress, as internal command buffer runs out of space.
    if (tid % 1024 == 0 && tid < 1024 * 4) {
        dbg::IsEnabled = true;
        dbg::Text("Hello from thread %d", tid);
    }
    dbg::IsEnabled = (tid == 0);

    // Standard inputs and text
    float time = dbg::GetTime();
    dbg::Text("Time: %.1f Frame: %d", time, dbg::GetFrameCount());

    // Graphing of 1D expressions
    dbg::Plot("Sin(x)", (float x) => sin(x + time));
    dbg::Plot("Sqr(x)", (float x) => fract(floor(x + time) * 0.5) * 4 + 2);
    dbg::Plot("Saw(x)", (float x) => abs(fract(x + time) * 4 - 2) - 4);

    // Stateful widgets are shared per label/ID and can be read or added by any invocation, even when `IsEnabled=false`.
    // We are dispatching one million invocations for stress testing, so the counter should increment accordingly.
    if (dbg::Button("Click me")) {
        InterlockedAdd(storage[0], 1);
        dbg::PauseAfterFrame();
    }
    if (storage[0] > 0) {
        dbg::Text("Seen by %d threads\n", storage[0]);
    }
    int shapeCount = (int)dbg::Drag("Shape Count", 100, 1, 1000, "%.0f");

    if (!dbg::Checkbox("Animate", true)) time = 0;

    if (dbg::Checkbox("Show Normals", true)) {
        int2 screenPos = dbg::GetPickedTID().xy;
        float depth = depthBuffer.Load(screenPos);

        float4 ndcPos = mul(invProj, float4(screenPos, depth, 1));
        float3 worldPos = ndcPos.xyz / ndcPos.w + viewOrigin;
        float3 normal = normalBuffer.Load(screenPos).xyz * 2 - 1;

        dbg::SetFill(1, 1, 1);
        dbg::DrawText3D(worldPos, "\nPosition: %.3v3f\nNormal: %.1v3f", worldPos, normal);
        dbg::SetStroke(float4(1, 1, 0, 1));
        dbg::DrawArrow3D(worldPos + normal * 0.04, normal);
    }

    // 3D shapes
    if (tid < shapeCount) {
        dbg::IsEnabled = true;
        float3 color = havk::colorutils::HSV_to_sRGB(float3(fract(tid / 9.66), 0.9, 0.9));

        if (tid < 4) {
            dbg::PushID(tid);
            dbg::Separator("Shape %d", tid);
            color = dbg::ColorEdit("Color", color);
            dbg::PopID();
        }

        dbg::PushTransform();
        dbg::RotateX(time * (tid % 2 == 0 ? 0.2 : -0.5) + tid * 3.56);
        dbg::RotateY(time * (tid % 2 == 0 ? 0.5 : -0.8) + tid * 7.62);

        float3 pos = float3(tid / 5, 0, tid % 5) * 5;
        float radius = 1 + abs(sin(time * 3.14159));

        if ((tid & 2) == 0) {
            dbg::SetFill(color);
        } else {
            // Wireframe mode
            dbg::NoFill();  // same as SetFill(0)
            dbg::SetStroke(float4(color, 1));
        }

        if ((tid & 1) == 0) {
            float3 size = 1;
            size[(int)time % 3] = radius;
            dbg::DrawCube3D(pos - size, pos + size);
        } else {
            dbg::DrawSphere3D(pos, radius);
        }
        dbg::PopTransform();
    }

    // 2D shapes
    if (tid == 0) {
        if (dbg::IsMouseDown(dbg::MouseButton::Left)) {
            float2 clickPos = dbg::GetMousePos(dbg::ButtonState::Pressed);

            dbg::SetStroke(float3(0.3, 0.5, 1.0), 3.0);
            dbg::DrawLine2D(dbg::GetMousePos(), clickPos);

            dbg::SetFill(1, 1, 1);
            dbg::DrawText2D(clickPos, "Clicked here");
        }
    }
}

[numthreads(8, 8)]
void CS_ComposeDebug(
    uniform ImageHandle2D<float4> colorBuffer,
    uniform TextureHandle2D<float4> normalBuffer,
    uniform TextureHandle2D<float> depthBuffer,
    uniform float3 viewOrigin,
    uniform float4x4 invProj,
    uint2 screenPos: SV_DispatchThreadID)
{
    if (any(screenPos >= colorBuffer.Size)) return;

    float depth = depthBuffer.Load(screenPos);
    float4 color = float4(0.5, 0.6, 0.7, 1.0);

    if (depth > 0) {
        color = colorBuffer.Load(screenPos);
        float3 normal = normalBuffer.Load(screenPos).xyz * 2 - 1;

        color.rgb *= max(0.1, dot(normal, normalize(float3(-0.2, 1.0, 0.8)))) +
                     max(0.1, dot(normal, normalize(float3(0.2, -0.3, -0.8))));

        float4 ndcPos = mul(invProj, float4(screenPos, depth, 1));
        float3 worldPos = ndcPos.xyz / ndcPos.w + viewOrigin;

        bool3 checker = fract(worldPos + 0.0001) < 0.5;
        float3 c = select(checker.yzx ^ checker.zxy, 1.0, 0.8);
        float3 m = pow(abs(normal), 20.0);
        color.rgb *= (c.x * m.x + c.y * m.y + c.z * m.z) / (m.x + m.y + m.z);
    }
    colorBuffer.Store(screenPos, color);
}
