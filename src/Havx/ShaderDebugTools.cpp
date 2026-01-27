#include "ShaderDebugTools.h"
#include "SystemUtils.h"
#include "Yson.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include "Shaders/Havk/DebugTools.h"

#include <bit>
#include <map>

using namespace havk::vectors;
namespace shbind = havx::shader::dbg;

namespace havx {

static constexpr uint32_t kConstId_ContextPtr = 991, kConstId_ProgramId = 992, kConstId_MaxWidgetSlots = 993;
static constexpr uint32_t kMaxWidgetSlots = 2048; // MUST be a power of two.
static constexpr int kPickerGridSize = 20;
static constexpr int kPlotStorageSize = 65536;

enum class CommandType : uint8_t {
    None,
    FailAssert, SyncWidget,
    UI_Separator, UI_Text,  // transient (no hash slot)
    UI_Button, UI_Checkbox, UI_Combo,
    UI_Slider,
    UI_Drag1, UI_Drag2, UI_Drag3, UI_Drag4,
    UI_ColorEdit, UI_Plot,
    G_SetColor, G_SetTransform, G_PushTransform, G_PopTransform,
    G_Translate, G_Scale, G_RotateX, G_RotateY, G_RotateZ,
    G_Line, G_Cube, G_Sphere, G_Text,

    G_ShapeFirst = G_Line, G_ShapeCount = G_Text - G_ShapeFirst + 1,
};
using StringId = uint32_t;

static float asfloat(uint32_t x) { return std::bit_cast<float>(x); }

struct ProgramData {
    std::vector<std::pair<uint32_t, std::string>> StringDefs;
    std::string SourcePath;
};

struct ShapeDrawBatch {
    CommandType ShapeType;
    uint32_t Count, Capacity;
    uint32_t StorageOffset;
};
struct ShapeBuilder {
    static constexpr float4x3 kIdentityTransform = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 0, 0, 0 } };
    static constexpr uint32_t kMaxBatchSize = 4096;

    uint32_t FillColor;
    uint32_t StrokeColor;
    float StrokeWidth;

    bool TransformChanged;
    uint32_t StackDepth = 0;
    float4x3 TransformStack[16] = { kIdentityTransform };

    ShapeDrawBatch PendingBatches[(int)CommandType::G_ShapeCount] = {};
    havk::BufferPtr StorageBuffer;
    havk::BufferSpan<shbind::ShapeInstance> InstanceBuffer;
    havk::BufferSpan<float3x4> TransformBuffer;
    uint32_t InstanceIdx = 0, TransformIdx = 0;

    float4x4 ProjViewMat;

    std::vector<ShapeDrawBatch> DrawBatches;

    void CreateBuffers(havk::DeviceContext* device) {
        const uint32_t maxInstances = 1024 * 128;
        const uint32_t maxTransforms = 1024 * 32;
        size_t storageSize = maxInstances * sizeof(shbind::ShapeInstance) + maxTransforms * sizeof(float3x4);
        StorageBuffer = device->CreateBuffer(storageSize, havk::BufferFlags::HostMem_Cached);

        auto span = StorageBuffer->Slice<uint8_t>();
        InstanceBuffer = span.bump_slice<shbind::ShapeInstance>(maxInstances);
        TransformBuffer = span.as<float3x4>();
    }

    bool IsOutOfSpace() {
        return InstanceIdx + kMaxBatchSize >= InstanceBuffer.size() ||
               TransformIdx + 1 >= TransformBuffer.size();
    }

    void Reset() {
        ResetBrush();

        memset(PendingBatches, 0, sizeof(PendingBatches));
        InstanceIdx = 0;
        TransformIdx = 0;

        DrawBatches.clear();
    }
    void ResetBrush() {
        FillColor = 0x00'000000;
        StrokeColor = 0xFF'FFFFFF;
        StrokeWidth = 1.0f;

        TransformChanged = true;
        StackDepth = 0;
        TransformStack[0] = kIdentityTransform;
    }

    void Add(CommandType type, const uint32_t* args, std::string_view textArg) {
        if (TransformChanged) {
            TransformChanged = false;
            memcpy(&TransformBuffer[++TransformIdx], &TransformStack[StackDepth], sizeof(float3x4)); // mat3x4 == float4x3
        }
        if ((args[2] & 0x7FFFFFFF) > 0x7F800000) { // NaN
            DrawShape2D(type, args, textArg);
            return;
        }
        shbind::ShapeInstance instance = { .Color = FillColor, .TransformIdx = TransformIdx };

        // TODO: consider using https://prideout.net/shapes
        switch (type) {
            case CommandType::G_Line: {
                instance.Color = StrokeColor;
                memcpy(instance.Pos, args, 6 * sizeof(float));
                break;
            }
            case CommandType::G_Cube: {
                memcpy(instance.Pos, args, 6 * sizeof(float));
                break;
            }
            case CommandType::G_Sphere: {
                memcpy(instance.Pos, args, 4 * sizeof(float));
                break;
            }
            case CommandType::G_Text: {
                memcpy(instance.Pos, args, 3 * sizeof(float));
                float4 clipPos = ProjViewMat * float4x4(TransformStack[StackDepth]) * float4(instance.Pos[0], 1);
                bool visible = clipPos.x > -clipPos.w && clipPos.x < +clipPos.w &&  //
                               clipPos.y > -clipPos.w && clipPos.y < +clipPos.w &&  //
                               clipPos.z > -clipPos.w && clipPos.z < +clipPos.w;
                if (visible) {
                    ImGuiViewport* viewport = ImGui::GetMainViewport();
                    ImDrawList* drawList = ImGui::GetBackgroundDrawList(viewport);
                    ImFont* font = ImGui::GetFont();

                    ImVec2 halfScreen = ImGui::GetIO().DisplaySize * 0.5f;
                    ImVec2 screenPos = ImVec2(clipPos.x / clipPos.w, clipPos.y / clipPos.w) * halfScreen + halfScreen;
                    float fontSize = glm::clamp(80.0f / clipPos.w, roundf(ImGui::GetFontSize() * 0.75f), roundf(ImGui::GetFontSize() * 2.5f));

                    ImVec2 textSize = font->CalcTextSizeA(fontSize, halfScreen.x, 0.0f, textArg.data(), textArg.data() + textArg.size());
                    screenPos.x -= textSize.x * 0.5f;
                    screenPos += viewport->Pos;
                    drawList->AddText(font, fontSize, screenPos, ImColSwap(FillColor), textArg.data(), textArg.data() + textArg.size());
                }
                return;
            }
            default: HAVK_ASSERT(!"Unknown shape type"); break;
        }

        // Do wireframe for NoFill()
        if (FillColor < 0x80'000000 && type == CommandType::G_Cube) {
            AddCubeWireframe(instance.Pos[0], instance.Pos[1]);
        } else if (FillColor < 0x80'000000 && type == CommandType::G_Sphere) {
            AddSphereWireframe(instance.Pos[0], instance.Pos[1].x);
        } else {
            AddInstance(type, instance);
        }
    }

    void AddInstance(CommandType type, const shbind::ShapeInstance& obj) {
        auto& batch = PendingBatches[(int)type - (int)CommandType::G_ShapeFirst];
        if (batch.Count >= batch.Capacity) {
            if (batch.Count > 0) {
                DrawBatches.push_back(batch);
            }
            HAVK_ASSERT(InstanceIdx + kMaxBatchSize < InstanceBuffer.size());

            batch = { .ShapeType = type, .Count = 0, .Capacity = kMaxBatchSize, .StorageOffset = InstanceIdx };
            InstanceIdx += batch.Capacity;
        }
        InstanceBuffer[batch.StorageOffset + batch.Count] = obj;
        batch.Count++;
    }

    void AddCubeWireframe(float3 p1, float3 p2) {
        for (int i = 0; i < 2; i++) {
            float y = i == 0 ? p1.y : p2.y;
            AddInstance(CommandType::G_Line, { { float3(p1.x, y, p1.z), float3(p2.x, y, p1.z) }, StrokeColor, TransformIdx });
            AddInstance(CommandType::G_Line, { { float3(p2.x, y, p1.z), float3(p2.x, y, p2.z) }, StrokeColor, TransformIdx });
            AddInstance(CommandType::G_Line, { { float3(p2.x, y, p2.z), float3(p1.x, y, p2.z) }, StrokeColor, TransformIdx });
            AddInstance(CommandType::G_Line, { { float3(p1.x, y, p2.z), float3(p1.x, y, p1.z) }, StrokeColor, TransformIdx });
        }
        for (int i = 0; i < 4; i++) {
            float x = (i & 1) ? p1.x : p2.x;
            float z = (i & 2) ? p1.z : p2.z;
            AddInstance(CommandType::G_Line, { { float3(x, p1.y, z), float3(x, p2.y, z) }, StrokeColor, TransformIdx });
        }
    }
    void AddSphereWireframe(float3 center, float radius) {
        float su0 = 0, cu0 = 1;

        for (int i = 1; i <= 24; i++) {
            float u = i / 24.0 * M_PI * 2;
            float su1 = sin(u), cu1 = cos(u);

            for (int j = 1; j < 8; j++) {
                float v = (j / 8.0) * M_PI;
                float sv = sin(v) * radius, cv = cos(v) * radius;
                float3 p1 = float3(su0 * sv, cv, cu0 * sv);
                float3 p2 = float3(su1 * sv, cv, cu1 * sv);

                AddInstance(CommandType::G_Line, { { center + float3(p1.x, p1.y, p1.z), center + float3(p2.x, p2.y, p2.z) }, StrokeColor, TransformIdx });
                AddInstance(CommandType::G_Line, { { center + float3(p1.y, p1.z, p1.x), center + float3(p2.y, p2.z, p2.x) }, StrokeColor, TransformIdx });
                AddInstance(CommandType::G_Line, { { center + float3(p1.z, p1.x, p1.y), center + float3(p2.z, p2.x, p2.y) }, StrokeColor, TransformIdx });
            }
            su0 = su1, cu0 = cu1;
        }
    }

    void DrawShape2D(CommandType type, const uint32_t* args, std::string_view textArg) {
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImDrawList* drawList = ImGui::GetBackgroundDrawList(viewport);
        ImVec2 v1 = ImVec2(asfloat(args[0]), asfloat(args[1])) + viewport->Pos;
        ImVec2 v2 = ImVec2(asfloat(args[3]), asfloat(args[4])) + viewport->Pos;

        if (type == CommandType::G_Line) {
            if (StrokeColor >= 0x01000000) drawList->AddLine(v1, v2, ImColSwap(StrokeColor), StrokeWidth);
        } else if (type == CommandType::G_Cube) {
            if (FillColor >= 0x01000000) drawList->AddRectFilled(v1, v2, ImColSwap(FillColor));
            if (StrokeColor >= 0x01000000) drawList->AddRect(v1, v2, ImColSwap(StrokeColor), 0, 0, StrokeWidth);
        } else if (type == CommandType::G_Sphere) {
            float radius = asfloat(args[3]);
            if (FillColor >= 0x01000000) drawList->AddCircleFilled(v1, radius, ImColSwap(FillColor));
            if (StrokeColor >= 0x01000000) drawList->AddCircle(v1, radius, ImColSwap(StrokeColor), 0, StrokeWidth);
        } else if (type == CommandType::G_Text) {
            drawList->AddText(v1, ImColSwap(FillColor), textArg.data(), textArg.data() + textArg.size());
        } else {
            HAVK_ASSERT(!"Unknown shape type");
        }
    }

    // 0xAARRGGBB -> whatever ImGui uses
    static ImU32 ImColSwap(uint32_t argb) {
#ifdef IMGUI_USE_BGRA_PACKED_COLOR
        return argb;
#endif
        return (argb & 0xFF00FF00) | (argb >> 16 & 0xFF) | (argb << 16 & 0xFF0000);
    }

    void UpdateTransform(CommandType op, const uint32_t* args) {
        float4x3& curr = TransformStack[StackDepth];

        switch (op) {
            case CommandType::G_SetTransform: {
                float3x3 v;
                memcpy(&v, args, sizeof(v));
                curr = { v[0], v[1], v[2], curr[3] };
                break;
            }
            case CommandType::G_PushTransform: {
                HAVK_ASSERT(StackDepth + 1 < std::size(TransformStack));
                TransformStack[++StackDepth] = curr;
                break;
            }
            case CommandType::G_PopTransform: {
                if (StackDepth > 0) {
                    StackDepth--;
                } else {
                    TransformStack[0] = kIdentityTransform;
                }
                break;
            }
            case CommandType::G_Translate: {
                float3 v;
                memcpy(&v, args, sizeof(v));
                curr[3] += v;
                break;
            }
            case CommandType::G_Scale: {
                float3 v;
                memcpy(&v, args, sizeof(v));
                curr[0] *= v.x;
                curr[1] *= v.y;
                curr[2] *= v.z;
                break;
            }
            case CommandType::G_RotateX:
            case CommandType::G_RotateY:
            case CommandType::G_RotateZ: {
                float a = asfloat(args[0]);
                float c = cos(a), s = sin(a);

                float3x3 rot = float3x3(curr);
                if (op == CommandType::G_RotateX) rot = Mul(rot, { { 1, 0, 0 }, { 0, c, s }, { 0, -s, c } });
                if (op == CommandType::G_RotateY) rot = Mul(rot, { { c, 0, -s }, { 0, 1, 0 }, { s, 0, c } });
                if (op == CommandType::G_RotateZ) rot = Mul(rot, { { c, s, 0 }, { -s, c, 0 }, { 0, 0, 1 } });
                curr = { rot[0], rot[1], rot[2], curr[3] };
                break;
            }
            default: HAVK_ASSERT(!"Unknown geometry command"); break;
        }
        TransformChanged = true;
    }
    static float3x3 Mul(const float3x3& a, const float3x3& b) {
        float3 r0 = a[0] * b[0][0] + a[1] * b[0][1] + a[2] * b[0][2];
        float3 r1 = a[0] * b[1][0] + a[1] * b[1][1] + a[2] * b[1][2];
        float3 r2 = a[0] * b[2][0] + a[1] * b[2][1] + a[2] * b[2][2];
        return { r0, r1, r2 };
    }
};

struct ShadebugContext {
    havk::DeviceContext* Device;
    std::vector<ProgramData> Programs;

    havk::BufferPtr StorageBuffer;

    havk::AttachmentLayout CurrDrawAttachLayout;
    havk::GraphicsPipelinePtr DrawCubePipeline, DrawLinePipeline, DrawSpherePipeline;
    havk::BufferPtr CubeIndexBuffer;

    ShapeBuilder ShapeBuf;

    ImVec2 MouseLastDownPos, MouseLastClickedPos, MouseLastReleasedPos;

    int2 PickerSelectedPos = { 0, 0 };
    int2 PickerDragOrigin = { 0, 0 };
    bool ShowPicker = false;
    bool PauseFrame = false;

    ShadebugContext(havk::DeviceContext* device) : Device(device) {
        uint32_t extraSize = kPickerGridSize * kPickerGridSize * 4 + kPlotStorageSize * sizeof(float);
        StorageBuffer = device->CreateBuffer(1024 * 1024 * 16 + extraSize, havk::BufferFlags::HostMem_Cached);
        memset(StorageBuffer->MappedData, 0, StorageBuffer->Size);

        device->OnCreatePipelineHook_ = [this](havk::Span<const havk::ModuleDesc> mods, VkBaseInStructure* createInfo,
                                               VkPipelineShaderStageCreateInfo* stages, VkPipeline* pipeline) {
            ProgramData* progData = LoadProgramMetadata(mods[0].SourcePath);

            havk::SpecConstMap specMap;
            VkSpecializationInfo newSpecInfo;

            if (progData != nullptr) {
                auto* currSpec = stages[0].pSpecializationInfo;
                if (currSpec->mapEntryCount != 0) {
                    specMap.Entries = std::vector(currSpec->pMapEntries, currSpec->pMapEntries + currSpec->mapEntryCount);
                    specMap.ConstantData = std::vector((const uint8_t*)currSpec->pData,
                                                       (const uint8_t*)currSpec->pData + currSpec->dataSize);
                }
                specMap.Add(kConstId_ContextPtr, StorageBuffer->DeviceAddress);
                specMap.Add(kConstId_ProgramId, (uint32_t)(progData - Programs.data()));
                specMap.Add(kConstId_MaxWidgetSlots, kMaxWidgetSlots);

                newSpecInfo = specMap.GetSpecInfo();
                for (uint32_t i = 0; i < mods.size(); i++) {
                    HAVK_ASSERT(stages[i].pSpecializationInfo == currSpec);
                    stages[i].pSpecializationInfo = &newSpecInfo;
                }
            }

            if (createInfo->sType == VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO) {
                return vkCreateComputePipelines(Device->Device, Device->PipelineCache, 1,  //
                                                (VkComputePipelineCreateInfo*)createInfo, nullptr, pipeline);
            }
            if (createInfo->sType == VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO) {
                return vkCreateGraphicsPipelines(Device->Device, Device->PipelineCache, 1,  //
                                                 (VkGraphicsPipelineCreateInfo*)createInfo, nullptr, pipeline);
            }
            HAVK_ASSERT(!"Implement me");
            return VK_ERROR_FEATURE_NOT_PRESENT;
        };
        device->OnDestroyPipelineHook_ = [this](havk::Pipeline& pipe) {
            // TODO: avoid leaking ProgramData instances
        };
    }
    ~ShadebugContext() {
        Device->OnCreatePipelineHook_ = nullptr;
        Device->OnDestroyPipelineHook_ = nullptr;
    }

    ProgramData* LoadProgramMetadata(const char* sourcePath) {
        if (!sourcePath || strcmp(sourcePath, "Shaders/Havk/DebugTools.slang") == 0) return nullptr;

        std::string path = havx::GetExecFilePath();
        while (!path.empty() && !path.ends_with('/') && !path.ends_with('\\')) path.pop_back();
        path += "shader-build/";
        path += sourcePath;

        while (!path.empty() && !path.ends_with('.')) path.pop_back();
        path += "reflect.json";

        auto reflectJson = havx::ReadFileBytes(path);
        if (reflectJson.empty()) return nullptr;

        auto reflectJsonStr = std::string_view { (char*)reflectJson.data(), reflectJson.size() };
        if (reflectJsonStr.find("havk__DebugToolsCtx") == std::string::npos) return nullptr;

        auto reader = yson::Reader(reflectJsonStr);
        reader.ReadExpect(yson::kTypeObject);

        auto& program = Programs.emplace_back();
        program.SourcePath = sourcePath;

        while (reader.ReadNext()) {
            if (reader.MatchObject("hashedStrings")) {
                while (reader.ReadNext()) {
                    HAVK_ASSERT(reader.Type == yson::kTypeNumber);

                    std::string str;
                    yson::Unescape(reader.Key, str);
                    program.StringDefs.push_back({ (uint32_t)reader.GetInt(), str });
                }
            } else {
                reader.Skip();
            }
        }
        return &program;
    }

    const char* GetProgramString(uint32_t programId, uint32_t stringId) {
        for (auto& entry : Programs[programId].StringDefs) {
            if (entry.first == stringId) return entry.second.c_str();
        }
        return "";
    }

    void NewFrame(havk::CommandList& cmds) {
        shbind::FrameDebugContext ctx = {};
        ctx.SelectedTID = uint16_t2(PickerSelectedPos);
        ctx.EnableOncePID = UINT32_MAX;
        ctx.WantPauseAfterFrame = false;

        ImGuiIO& io = ImGui::GetIO();
        ctx.Time = (float)std::fmod(ImGui::GetTime(), 86400);
        ctx.FrameCount = ImGui::GetFrameCount();
        ctx.MainDisplaySize = float2(io.DisplaySize.x, io.DisplaySize.y);

        for (uint32_t i = 0; i < ImGuiKey_NamedKey_COUNT; i++) {
            ImGuiKeyData data = io.KeysData[i];
            uint32_t mask = 1u << (i & 31);
            ctx.KeyDown[i / 32] |= data.Down ? mask : 0;
            ctx.KeyPressed[i / 32] |= data.DownDuration == 0 ? mask : 0;
            ctx.KeyReleased[i / 32] |= !data.Down && data.DownDurationPrev > 0 ? mask : 0;
        }
        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 mousePos = io.MousePos - viewport->Pos;

        if (io.WantCaptureMouse) {
            for (uint32_t i = ImGuiKey_Mouse_BEGIN; i < ImGuiKey_Mouse_END; i++) {
                uint32_t j = i - ImGuiKey_NamedKey_BEGIN;
                uint32_t mask = 1u << (j & 31);
                ctx.KeyDown[j / 32] &= ~mask;
                ctx.KeyPressed[j / 32] &= ~mask;
                ctx.KeyReleased[j / 32] &= ~mask;
            }
        } else {
            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) MouseLastDownPos = mousePos;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) MouseLastClickedPos = mousePos;
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) MouseLastReleasedPos = mousePos;
            ctx.MouseWheel = { io.MouseWheelH, io.MouseWheel };
        }
        ctx.MousePos[0] = { mousePos.x, mousePos.y };
        ctx.MousePos[1] = { MouseLastDownPos.x, MouseLastDownPos.y };
        ctx.MousePos[2] = { MouseLastClickedPos.x, MouseLastClickedPos.y };
        ctx.MousePos[3] = { MouseLastReleasedPos.x, MouseLastReleasedPos.y };

        auto buffer = StorageBuffer->Slice<uint32_t>();
        auto contextData = buffer.bump_slice<shbind::FrameDebugContext>(1);

        ctx.WidgetHashTable = buffer.bump_slice(kMaxWidgetSlots);
        auto widgetData = buffer.bump_slice<shbind::WidgetState>(kMaxWidgetSlots);
        ctx.PlotData = buffer.bump_slice<float>(kPlotStorageSize);
        auto pixelPickData = buffer.bump_slice(kPickerGridSize * kPickerGridSize);

        ctx.CmdBufferPos = 0;
        ctx.CmdBufferEnd = buffer.size();
        ctx.CommandData = buffer;

        cmds.UpdateBuffer(*StorageBuffer, 0, sizeof(shbind::FrameDebugContext), &ctx);
        cmds.Barrier({ .SrcStages = VK_PIPELINE_STAGE_TRANSFER_BIT });
    }

    void DrawFrame(havk::CommandList& cmds, const Shadebug::DrawFrameParams& pars) {
        // Wait and process commands from previous frame
        Device->WaitIdle();
        StorageBuffer->Invalidate(0, VK_WHOLE_SIZE);

        auto buffer = StorageBuffer->Slice<uint32_t>();
        auto ctx = *buffer.bump_slice<shbind::FrameDebugContext>(1).data();  // copy
        auto widgetKeys = buffer.bump_slice(kMaxWidgetSlots);
        auto widgetData = buffer.bump_slice<shbind::WidgetState>(kMaxWidgetSlots);
        auto plotData = buffer.bump_slice<float>(kPlotStorageSize);
        auto pixelPickData = buffer.bump_slice(kPickerGridSize * kPickerGridSize);

        ImGui::Begin("ShaderDebugTools");

        if (ImGui::Button("Reset Widgets")) {
            memset(widgetKeys.data(), 0, widgetKeys.size_bytes());
        }

        ImGui::SameLine();
        ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_F, ImGuiInputFlags_RouteGlobal);
        if (ctx.WantPauseAfterFrame) {
            PauseFrame = true;
            ImGui::NavHighlightActivated(ImGui::GetID("Pause Shapes"));
        }
        ImGui::Checkbox("Pause Shapes", &PauseFrame);

        ImGui::SetNextItemWidth(6 * ImGui::GetFontSize());
        ImGui::DragInt2("Selected TID", &PickerSelectedPos.x, 0.5f);

        if (pars.PickImage != nullptr) {
            ImGui::SameLine();
            ImGui::SetNextItemShortcut(ImGuiMod_Ctrl | ImGuiKey_G, ImGuiInputFlags_RouteGlobal);
            ImGui::Checkbox("Show Picker", &ShowPicker);

            if (ctx.FrameCount == 0) {
                PickerSelectedPos = int2(pars.PickImage->Size / 2u);
            }
            const int numCells = kPickerGridSize - 1 + (kPickerGridSize % 2);
            const int radius = numCells / 2;

            // Read image for picking before we start drawing,
            // since we allow `pars.ColorBuffer == pars.PickImage`
            cmds.Barrier({ .DstStages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT });
            cmds.Dispatch<shbind::CS_CopyImageToRGBA8>({ kPickerGridSize, kPickerGridSize, 1 }, {
                .srcImage = *pars.PickImage,
                .dstBuffer = pixelPickData,
                .srcRect = { PickerSelectedPos.x - radius, PickerSelectedPos.y - radius, kPickerGridSize, kPickerGridSize },
                .dstStride = kPickerGridSize,
            });
        }
        ImGui::Separator();

        if (ShapeBuf.StorageBuffer == nullptr) {
            ShapeBuf.CreateBuffers(Device);
        } else if (!PauseFrame) {
            ShapeBuf.Reset();
        }
        float4x4 proj = pars.ProjMat;
        proj[3] -= proj[0] * pars.ViewOrigin.x + proj[1] * pars.ViewOrigin.y + proj[2] * pars.ViewOrigin.z; // translate to -O
        ShapeBuf.ProjViewMat = proj;

        std::vector<const shbind::Command*> activePlots;

        uint32_t cmdEndPos = std::min(ctx.CmdBufferPos, ctx.CmdBufferEnd);

        IterateCommandGroups(buffer.data(), cmdEndPos, [&](const shbind::Command& cmd) {
            auto type = (CommandType)cmd.Type;

            switch (type) {
                case CommandType::FailAssert: {
                    const char* msg = GetProgramString(cmd.ProgramId, cmd.Data[0]);
                    const char* file = GetProgramString(cmd.ProgramId, cmd.Data[1]);
                    uint32_t lineNo = cmd.Data[2];
                    Device->Log(havk::LogLevel::Error, "Shader assert failed: '%s'  (%s:%d TID=[%d %d])",  //
                                msg, file, lineNo, cmd.ThreadId.x, cmd.ThreadId.y);
                    break;
                }
                case CommandType::SyncWidget: {
                    // TODO: evict widget slots unused after some frames
                    auto type = (CommandType)(cmd.Data[0] & 255);
                    shbind::WidgetState& state = widgetData[cmd.Data[0] >> 8].value;

                    if (type == CommandType::UI_Plot) {
                        activePlots.push_back(&cmd);
                        break;
                    }
                    ImGui::PushID(&state);
                    DrawWidget(type, cmd.ProgramId, state);
                    ImGui::PopID();
                    break;
                }
                case CommandType::UI_Text:
                case CommandType::UI_Separator: {
                    const char* fmt = GetProgramString(cmd.ProgramId, cmd.Data[0]);
                    std::string text = FormatProgramString(fmt, cmd.ProgramId, &cmd.Data[1]);

                    if (type == CommandType::UI_Text) {
                        ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
                    } else {
                        ImGui::SeparatorText(text.c_str());
                    }
                    break;
                }
                case CommandType::G_SetColor: {
                    if (pars.ColorBuffer == nullptr || PauseFrame) break;

                    ShapeBuf.FillColor = cmd.Data[0];
                    ShapeBuf.StrokeColor = cmd.Data[1];
                    ShapeBuf.StrokeWidth = asfloat(cmd.Data[2]);
                    break;
                }
                case CommandType::G_SetTransform:
                case CommandType::G_PushTransform:
                case CommandType::G_PopTransform:
                case CommandType::G_Translate:
                case CommandType::G_Scale:
                case CommandType::G_RotateY:
                case CommandType::G_RotateX:
                case CommandType::G_RotateZ: {
                    if (pars.ColorBuffer == nullptr || PauseFrame) break;

                    ShapeBuf.UpdateTransform(type, cmd.Data);
                    break;
                }
                case CommandType::G_Line:
                case CommandType::G_Cube:
                case CommandType::G_Sphere: {
                    if (pars.ColorBuffer == nullptr || PauseFrame || ShapeBuf.IsOutOfSpace()) break;

                    ShapeBuf.Add(type, cmd.Data, "");
                    break;
                }
                case CommandType::G_Text: {
                    if (pars.ColorBuffer == nullptr || PauseFrame || ShapeBuf.IsOutOfSpace()) break;

                    const char* fmt = GetProgramString(cmd.ProgramId, cmd.Data[3]);
                    std::string text = FormatProgramString(fmt, cmd.ProgramId, &cmd.Data[4]);
                    ShapeBuf.Add(type, cmd.Data, text);
                    break;
                }
                default: HAVK_ASSERT(!"Unhandled command"); break;
            }
        });
        if (pars.ColorBuffer != nullptr && ShapeBuf.InstanceIdx > 0) {
            BeginDrawingShapes(cmds, pars);
            DrawShapes(cmds, pars);
            cmds.EndRendering();
        }

        if (activePlots.size() > 0) {
            const auto plotChildFlags = ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_ResizeY;
            const auto plotFlags = ImPlotFlags_Equal;

            auto minFrameSize = ImGui::GetContentRegionAvail();
            minFrameSize.y = std::max(minFrameSize.y, ImGui::GetTextLineHeight() * 25.0f);
            ImGui::BeginChild("##ShaderPlotFrame", minFrameSize, plotChildFlags);

            if (ImPlot::BeginPlot("##ShaderPlot", ImGui::GetContentRegionAvail(), plotFlags)) {
                ImPlot::PushStyleVar(ImPlotStyleVar_LineWeight, 3.0f);

                ImVec2 plotSize = ImPlot::GetPlotSize();
                ImPlotRect plotRange = ImPlot::GetPlotLimits();
                uint32_t nextDataOffset = 0;

                for (const shbind::Command* cmd : activePlots) {
                    shbind::WidgetState& state = widgetData[cmd->Data[0] >> 8].value;
                    const char* label = GetProgramString(cmd->ProgramId, state.Label);

                    float* data = &plotData[state.Params[0]].value;
                    uint32_t numSamples = state.Params[1];
                    float rangeMin = asfloat(state.Params[2]);
                    float rangeMax = asfloat(state.Params[3]);
                    float scale = (rangeMax - rangeMin) / (numSamples - 1);
                    ImPlot::PlotLine(label, data, numSamples, scale, rangeMin);

                    uint32_t numSamplesOptimal = std::max(std::min((uint32_t)(plotSize.x * 2 + 0.5), 4096u), 64u);
                    state.Params[0] = nextDataOffset;
                    state.Params[1] = numSamplesOptimal;
                    state.Params[2] = std::bit_cast<uint32_t>((float)plotRange.X.Min);
                    state.Params[3] = std::bit_cast<uint32_t>((float)plotRange.X.Max);
                    nextDataOffset += numSamplesOptimal;
                }
                ImPlot::PopStyleVar();
                ImPlot::EndPlot();
            }
            ImGui::EndChild();
        }

        ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImVec2 selectedPos = viewport->Pos + ImVec2(PickerSelectedPos.x, PickerSelectedPos.y);

        if (pars.PickImage != nullptr) {
            ImDrawList* bgdl = ImGui::GetBackgroundDrawList(viewport);
            bgdl->AddCircle(selectedPos, int(kPickerGridSize / 2) + 2, IM_COL32(192, 192, 192, 240), 0, 2.0f);
        }
        if (ShowPicker && pars.PickImage != nullptr) {
            const int numCells = kPickerGridSize - 1 + (kPickerGridSize % 2);
            const int radius = numCells / 2;
            
            const auto winFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoDecoration |
                                  ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoDocking;

            const float sq = roundf(ImGui::GetFontSize() * 0.7);
            ImVec2 viewSize = ImVec2(numCells, numCells) * sq;
            
            ImGui::SetNextWindowPos(selectedPos + ImVec2(6, 6));

            if (ImGui::Begin("##PixelPickerTooltip", nullptr, winFlags)) {
                uint32_t centerValU = pixelPickData[radius + radius * kPickerGridSize].value;
                float4 centerValF = float4(centerValU >> uint4(16, 8, 0, 24) & 255u) / 255.0f;
                ImGui::Text("%.3f %.3f %.3f %.3f", centerValF.x, centerValF.y, centerValF.z, centerValF.w);

                ImGui::Dummy(viewSize);
                ImDrawList* dl = ImGui::GetWindowDrawList();
                ImVec2 startPos = ImGui::GetItemRectMin();
                ImVec2 centerPos = startPos + ImVec2(radius, radius) * sq;

                for (uint32_t y = 0; y < numCells; y++) {
                    for (uint32_t x = 0; x < numCells; x++) {
                        ImVec2 pos = startPos + ImVec2(x, y) * sq;
                        uint4 color = pixelPickData[x + y * kPickerGridSize].value >> uint4(0, 8, 16, 24) & 255u;
                        dl->AddRectFilled(pos, pos + ImVec2(sq, sq), IM_COL32(color.x, color.y, color.z, color.w));
                    }
                }
                for (uint32_t i = 1; i < numCells; i++) {
                    ImVec2 r0 = startPos + ImVec2(0, i * sq);
                    ImVec2 c0 = startPos + ImVec2(i * sq, 0);
                    dl->AddLine(r0, r0 + ImVec2(numCells * sq, 0), IM_COL32(0, 0, 0, 40));
                    dl->AddLine(c0, c0 + ImVec2(0, numCells * sq), IM_COL32(0, 0, 0, 40));
                }
                int br = (centerValF.x * 0.3f + centerValF.y * 0.6f + centerValF.z * 0.1f) > 0.6 ? 0 : 255;
                dl->AddRect(centerPos - ImVec2(1, 1), centerPos + ImVec2(sq + 2, sq + 2), IM_COL32(br, br, br, 120), 2.0f);

                char centerText[32];
                snprintf(centerText, sizeof(centerText), "%d %d", PickerSelectedPos.x, PickerSelectedPos.y);
                ImVec2 textOffset = ImVec2((sq - ImGui::CalcTextSize(centerText).x) / 2, sq);
                dl->AddText(centerPos + textOffset, IM_COL32(br, br, br, 255), centerText);

                ImVec2 mousePos = ImGui::GetMousePos();

                // Dragging behaviors
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                    !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
                    PickerSelectedPos.x = mousePos.x - viewport->Pos.x;
                    PickerSelectedPos.y = mousePos.y - viewport->Pos.y;
                } else if (ImGui::IsItemClicked()) {
                    ImVec2 reloc = (mousePos - centerPos) / sq;
                    PickerSelectedPos.x += reloc.x;
                    PickerSelectedPos.y += reloc.y;
                } else if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && ImGui::IsWindowFocused()) {
                    ImVec2 delta = ImGui::GetMouseDragDelta() * 0.25f;
                    PickerSelectedPos.x = PickerDragOrigin.x + (int)delta.x;
                    PickerSelectedPos.y = PickerDragOrigin.y + (int)delta.y;
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                } else {
                    PickerDragOrigin = PickerSelectedPos;
                }
                PickerSelectedPos.x = std::min(std::max(PickerSelectedPos.x, 0), (int)pars.PickImage->Size.x);
                PickerSelectedPos.y = std::min(std::max(PickerSelectedPos.y, 0), (int)pars.PickImage->Size.y);

                if (ImGui::IsWindowHovered() && !ImGui::IsItemHovered()) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                }
            }
            ImGui::End();
        }
        ImGui::End();
    }

    // Iterate over commands, sorted per thread/program.
    void IterateCommandGroups(const uint32_t* cmdData, uint32_t dataLen, auto cb) {
        std::map<uint64_t, uint32_t> groups; // TODO-PERF: this can get quite expansive, but we really need predictable ordering
        uint64_t lastKey = UINT64_MAX;
        uint32_t* lastVal = nullptr;

        auto getKey = [](const shbind::Command& cmd) -> uint64_t {
            return (uint64_t)cmd.ThreadId.x | (uint64_t)cmd.ThreadId.y << 16 | (uint64_t)cmd.ProgramId << 32;
        };
        auto getAndIncrGroupOffset = [&](const shbind::Command& cmd) -> uint32_t {
            uint64_t key = getKey(cmd);
            if (lastKey != key) {
                lastKey = key;
                lastVal = &groups[key];
            }
            return (*lastVal)++;
        };

        // Count groups
        for (uint32_t pos = 0; pos < dataLen;) {
            auto cmd = (const shbind::Command*)&cmdData[pos];
            getAndIncrGroupOffset(*cmd);
            pos += cmd->Length;
        }
        if (groups.size() < 2) {
            // Fast path: one active thread, don't need another pass
            for (uint32_t pos = 0; pos < dataLen;) {
                auto cmd = (const shbind::Command*)&cmdData[pos];
                cb(*cmd);
                pos += cmd->Length;
            }
            return;
        }

        // Prefix sum into offsets
        uint32_t runningOffset = 0;
        for (auto& [key, offset] : groups) {
            uint32_t size = offset;
            offset = runningOffset;
            runningOffset += size;
        }

        // Scatter
        auto offsets = std::make_unique<uint32_t[]>(runningOffset);
        for (uint32_t pos = 0; pos < dataLen;) {
            auto cmd = (const shbind::Command*)&cmdData[pos];
            uint32_t groupOffset = getAndIncrGroupOffset(*cmd);
            offsets[groupOffset] = pos;
            pos += cmd->Length;
        }

        // Visit
        lastKey = UINT64_MAX;

        for (uint32_t i = 0; i < runningOffset; i++) {
            auto cmd = (const shbind::Command*)&cmdData[offsets[i]];

            uint64_t key = getKey(*cmd);
            if (lastKey != key) {
                lastKey = key;
                ShapeBuf.ResetBrush();
            }
            cb(*cmd);
        }
    }

    void DrawWidget(CommandType type, uint32_t programId, shbind::WidgetState& state) {
        const char* label = GetProgramString(programId, state.Label);
        bool changed = false;

        switch (type) {
            case CommandType::UI_Button: {
                changed = ImGui::Button(label);
                break;
            }
            case CommandType::UI_Checkbox: {
                changed = ImGui::CheckboxFlags(label, &state.Params[0], 1);
                break;
            }
            case CommandType::UI_Combo: {
                const char* srcOptions = GetProgramString(programId, state.Params[1]);
                char options[2048] = {};
                for (uint32_t i = 0; i < sizeof(options) - 1 && srcOptions[i] != '\0'; i++) {
                    options[i] = (srcOptions[i] == ';') ? '\0' : srcOptions[i];
                }
                changed = ImGui::Combo(label, (int*)&state.Params[0], options);
                break;
            }
            case CommandType::UI_Slider: {
                float* fargs = (float*)state.Params;
                const char* fmt = GetProgramString(programId, state.Params[3]);
                changed = ImGui::SliderFloat(label, &fargs[0], fargs[1], fargs[2], fmt);
                break;
            }
            case CommandType::UI_Drag1:
            case CommandType::UI_Drag2:
            case CommandType::UI_Drag3:
            case CommandType::UI_Drag4: {
                float* fargs = (float*)state.Params;
                float speed = (fargs[5] - fargs[4]) / 250;
                const char* fmt = GetProgramString(programId, state.Params[6]);
                int ncomp = (int)type - (int)CommandType::UI_Drag1 + 1;
                ImGui::DragScalarN(label, ImGuiDataType_Float, fargs, ncomp, speed, &fargs[4], &fargs[5], fmt);
                return;
            }
            case CommandType::UI_ColorEdit: {
                float* color = (float*)state.Params;
                changed = ImGui::ColorEdit4(label, color, ImGuiColorEditFlags_Float | (color[3] < 0 ? ImGuiColorEditFlags_NoAlpha : 0));
                break;
            }
            default: HAVK_ASSERT(!"Unknown widget type"); break;
        }
        if (changed) {
            state.Params[6] = (uint32_t)ImGui::GetFrameCount();
        }
    }

    void BeginDrawingShapes(havk::CommandList& cmds, const Shadebug::DrawFrameParams& pars) {
        havk::GraphicsPipelineState rasterState = {
            .Raster = { .FrontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .CullFace = VK_CULL_MODE_NONE },
            .Depth = { .TestOp = pars.DepthCompareOp },
        };
        havk::AttachmentLayout attachLayout = {
            .Formats = {
                pars.ColorBuffer ? pars.ColorBuffer->Format : VK_FORMAT_UNDEFINED,
                pars.NormalBuffer ? pars.NormalBuffer->Format : VK_FORMAT_UNDEFINED,
            },
            .DepthFormat = pars.DepthBuffer ? pars.DepthBuffer->Format : VK_FORMAT_UNDEFINED,
        };

        if (CurrDrawAttachLayout != attachLayout) {
            CurrDrawAttachLayout = attachLayout;
            DrawCubePipeline = Device->CreateGraphicsPipeline({ shbind::VS_DrawCube::Module, shbind::FS_DrawCube::Module },
                                                              rasterState, attachLayout);

            DrawSpherePipeline = Device->CreateGraphicsPipeline({ shbind::VS_DrawSphere::Module, shbind::FS_DrawSphere::Module },
                                                                rasterState, attachLayout);

            havk::GraphicsPipelineState lineRasterState = rasterState;
            lineRasterState.Raster = { .Topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST, .LineWidth = havk::dynamic_state };
            DrawLinePipeline = Device->CreateGraphicsPipeline({ shbind::VS_DrawLine::Module, shbind::FS_DrawLine::Module },
                                                              lineRasterState, attachLayout);
        }
        if (CubeIndexBuffer == nullptr) {
            static const uint8_t kCubeFaceIndices[36] = {
                0, 2, 1, 2, 3, 1,  // X+
                5, 4, 1, 1, 4, 0,  // Y+
                0, 4, 6, 0, 6, 2,  // Z+
                6, 5, 7, 6, 4, 5,  // X-
                2, 6, 3, 6, 7, 3,  // Y-
                7, 1, 3, 7, 5, 1,  // Z-
            };
            static_assert((ShapeBuilder::kMaxBatchSize * 8) < UINT16_MAX);
            CubeIndexBuffer = Device->CreateBuffer(ShapeBuilder::kMaxBatchSize * 36 * sizeof(uint16_t),
                                                   havk::BufferFlags::DeviceMem | havk::BufferFlags::MapSeqWrite);

            auto indices = CubeIndexBuffer->Slice<uint16_t>();
            for (uint32_t i = 0; i < indices.size(); i++) {
                uint32_t cubeId = (i / 36);
                indices[i] = (uint16_t)(kCubeFaceIndices[i % 36] + (cubeId << 3));
            }
        }

        cmds.BeginRendering({
            .Attachments = {
                havk::RenderAttachment::Overlay(*pars.ColorBuffer),
                pars.NormalBuffer ? havk::RenderAttachment::Overlay(*pars.NormalBuffer) : havk::RenderAttachment{},
            },
            .DepthAttachment = pars.DepthBuffer ? havk::RenderAttachment::Overlay(*pars.DepthBuffer) : havk::RenderAttachment{},
            .SrcStages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        });
    }

    void DrawShapes(havk::CommandList& cmds, const Shadebug::DrawFrameParams& pars) {
        // Flush pending batches
        for (uint32_t i = 0; i < (int)CommandType::G_ShapeCount; i++) {
            ShapeDrawBatch& batch = ShapeBuf.PendingBatches[i];
            if (batch.Count > 0) {
                ShapeBuf.DrawBatches.push_back(batch);
                batch = {};
            }
        }
        cmds.PushConstants(shbind::DrawParams {
            .Objects = ShapeBuf.InstanceBuffer,
            .Transforms = ShapeBuf.TransformBuffer,
            .ViewOrigin = pars.ViewOrigin,
            .ProjMat = ShapeBuf.ProjViewMat,
        });
        cmds.BindIndexBuffer(*CubeIndexBuffer, 0, VK_INDEX_TYPE_UINT16);

        for (ShapeDrawBatch& batch : ShapeBuf.DrawBatches) {
            if (batch.ShapeType == CommandType::G_Line) {
                // TODO: maybe actually record and apply line width properly
                vkCmdSetLineWidth(cmds.Handle, ShapeBuf.StrokeWidth);
                cmds.Draw(*DrawLinePipeline, { .NumVertices = batch.Count * 2, .VertexOffset = batch.StorageOffset * 2});
            } else if (batch.ShapeType == CommandType::G_Cube) {
                cmds.DrawIndexed(*DrawCubePipeline, { .NumIndices = batch.Count * 36, .VertexOffset = batch.StorageOffset * 8 });
            } else if (batch.ShapeType == CommandType::G_Sphere) {
                cmds.DrawIndexed(*DrawSpherePipeline, { .NumIndices = batch.Count * 36, .VertexOffset = batch.StorageOffset * 8 });
            } else {
                HAVK_ASSERT(!"TODO");
            }
        }
    }

    std::string FormatProgramString(const char* fmt, uint32_t programId, const uint32_t* argp) {
        std::string buf;

        for (char ch; (ch = *fmt++) != '\0';) {
            if (ch != '%') [[likely]] {
                buf.push_back(ch);
                continue;
            }
            char outfmt[32] = { '%' };
            uint32_t outfmtPos = 1;

            #define copyfmt(ch) {                              \
                if (outfmtPos >= sizeof(outfmt) - 1) goto Err; \
                outfmt[outfmtPos++] = ch; }

            // Flags
            while (*fmt == '+' || *fmt == '-' || *fmt == '0' || *fmt == '#' || *fmt == ' ') copyfmt(*fmt++);

            // Width
            while (*fmt >= '0' && *fmt <= '9') copyfmt(*fmt++);

            // Precision
            if (*fmt == '.') {
                copyfmt(*fmt++);
                while (*fmt >= '0' && *fmt <= '9') copyfmt(*fmt++);
            }

            // Vector size (ext)
            int numLanes = 1;
            if (*fmt == 'v') {
                fmt++;
                numLanes = (*fmt++ - '0');
            }

            bool isLong = false;
            while (*fmt == 'l') { isLong = true; fmt++; }

            char type = *fmt++;

            // Always promote integers to 64-bit to reduce number of cases we have to handle
            if (type == 'd' || type == 'u' || type == 'x' || type == 'X') {
                copyfmt('l');
                copyfmt('l');
            }
            copyfmt(type);
#undef copyfmt

            switch (type) {
                case 's': {
                    buf += GetProgramString(programId, *argp++);
                    break;
                }
                case 'd': case 'u': case 'x': case 'X': {
                    for (uint32_t i = 0; i < numLanes; i++) {
                        uint64_t val = *argp++;
                        if (isLong) val |= uint64_t(*argp++) << 32;
                        else if (type != 'u') val = (uint64_t)(int32_t)val; // sign extend

                        if (numLanes >= 2 && i != 0) buf.push_back(' ');

                        size_t pos = buf.size();
                        buf.resize(pos + 256);
                        int r = snprintf(buf.data() + pos, 256, outfmt, val);
                        buf.resize(pos + (size_t)r);
                    }
                    break;
                }
                case 'f': case 'g': {
                    for (uint32_t i = 0; i < numLanes; i++) {
                        float val = asfloat(*argp++);
                        if (numLanes >= 2 && i != 0) buf.push_back(' ');

                        size_t pos = buf.size();
                        buf.resize(pos + 256);
                        int r = snprintf(buf.data() + pos, 256, outfmt, val);
                        buf.resize(pos + (size_t)r);
                    }
                    break;
                }
                default: {
                    buf += "(bad fmt spec)";
                    goto Err;
                }
            }
        }
Err:
        return buf;
    }
};

static ShadebugContext* g_ctx = nullptr;

void Shadebug::Initialize(havk::DeviceContext* ctx) {
    HAVK_ASSERT(!g_ctx || g_ctx->Device == ctx);
    if (!g_ctx) g_ctx = new ShadebugContext(ctx);
}
void Shadebug::Shutdown() {
    if (!g_ctx) return;

    delete g_ctx;
    g_ctx = nullptr;
}
void Shadebug::NewFrame(havk::CommandList& cmds) {
    if (!g_ctx) return;
    HAVK_ASSERT(g_ctx->Device == cmds.Context);
    g_ctx->NewFrame(cmds);
}
void Shadebug::DrawFrame(havk::CommandList& cmds, const DrawFrameParams& pars) {
    if (g_ctx) g_ctx->DrawFrame(cmds, pars);
}

};  // namespace havx