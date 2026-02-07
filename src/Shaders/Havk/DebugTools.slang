import Havk.Core;
import Havk.Math;

[vk::constant_id(991)] uint64_t havk__DebugToolsCtx = 0;
[vk::constant_id(992)] uint32_t havk__DebugToolsProgramId = 0;
[vk::constant_id(993)] uint32_t havk__MaxWidgetSlots = 2048;  // MUST be a power of two.

namespace dbg {

public static bool IsEnabled = false;

static const let ctx = (FrameDebugContext*)havk__DebugToolsCtx;
static uint widgetStackedHash = 0;

typealias StringId = uint32_t;

struct FrameDebugContext {
    uint2 SelectedTID;
    uint EnableOncePID;
    bool WantPauseAfterFrame;

    float Time;
    float2 MainDisplaySize;
    uint32_t FrameCount;

    int16_t2 MousePos[4];
    float2 MouseWheel;

    uint32_t KeyDown[(Key.NamedKey_COUNT + 31) / 32];
    uint32_t KeyPressed[(Key.NamedKey_COUNT + 31) / 32];
    uint32_t KeyReleased[(Key.NamedKey_COUNT + 31) / 32];

    uint32_t CmdBufferPos, CmdBufferEnd;
    uint32_t NumFailedAsserts;
    uint32_t* WidgetHashTable; // Keys[], followed by WidgetState[]
    uint32_t* CommandData;
    float* PlotData;
};
struct WidgetState {
    StringId Label;
    uint32_t Params[7];
};
// This must be mirrored in host manually because binding generator does not support enums yet (TODO)
enum CommandType : uint8_t {
    None,
    FailAssert, SyncWidget,
    UI_Separator, UI_Text,  // transient (no hash slot)
    UI_Button, UI_Checkbox, UI_Combo,
    UI_Slider,
    UI_Drag1, UI_Drag2, UI_Drag3, UI_Drag4,
    UI_ColorEdit, UI_Plot,
    G_SetColor, G_SetTransform, G_PushTransform, G_PopTransform,
    G_Translate, G_Scale, G_RotateX, G_RotateY, G_RotateZ,
    G_Line, G_Cube, G_Sphere, G_Arrow, G_Text,
};
struct Command {
    uint8_t Length;
    uint8_t Type;
    uint16_t ProgramId;
    uint16_t2 ThreadId;
    uint32_t Data[];
};

Command* WriteCommand(CommandType type, uint argc) {
    uint pos = 0;
    uint length = argc + 2;
    InterlockedAdd(ctx->CmdBufferPos, length, pos);
    if (pos + length >= ctx->CmdBufferEnd) return nullptr;

    Command* cmd = (Command*)&ctx->CommandData[pos];
    cmd->Type = (uint8_t)type;
    cmd->Length = (uint8_t)length;
    cmd->ProgramId = (uint16_t)havk__DebugToolsProgramId;
    cmd->ThreadId = (uint16_t2)GetCurrentTID();
    return cmd;
}

#define EMIT_CMD(type, argc)                             \
    if (havk__DebugToolsCtx == 0 || !IsEnabled) return;  \
    Command* cmd = WriteCommand(CommandType.type, argc); \
    if (!cmd) return;

// TODO: would be interesting to see if [noinline] helps reduce compile bloat and perf, but slang spirv backend doesn't actually emit it

public void FailAssert(string message, string fileName, int lineNo) {
    if (havk__DebugToolsCtx == 0 || ctx.NumFailedAsserts >= 32) return;
    Command* cmd = WriteCommand(CommandType.FailAssert, 3);
    if (!cmd) return;
    InterlockedAdd(ctx->NumFailedAsserts, 1);
    cmd->Data[0] = getStringHash(message);
    cmd->Data[1] = getStringHash(fileName);
    cmd->Data[2] = lineNo;
}
WidgetState* FindWidget(CommandType type, StringId label, out bool justAdded) {
    uint hash = label ^ ((uint)type * 0x165667B1u) ^ widgetStackedHash;
    uint slot = hash & (havk__MaxWidgetSlots - 1);
    justAdded = false;

    // Avoid slow CAS probing loop in common case
    [branch] if (ctx->WidgetHashTable[slot] != hash) {
        bool found = false;
        [loop] for (uint i = 0; i < 8; i++) {
            uint prevKey;
            InterlockedCompareExchange(ctx->WidgetHashTable[slot], 0, hash, prevKey);

            if (prevKey == hash || prevKey == 0) {
                justAdded = (prevKey == 0);
                found = true;
                break;
            }
            slot = (slot + 1) & (havk__MaxWidgetSlots - 1);
        }
        if (!found) return nullptr;
    }
    let entry = (WidgetState*)&ctx->WidgetHashTable[slot * (sizeof(WidgetState) / 4) + havk__MaxWidgetSlots];

    if (justAdded) {
        entry->Label = label;
    }
    if (IsEnabled) {
        Command* sync = WriteCommand(CommandType.SyncWidget, 1);
        if (sync) sync.Data[0] = slot << 8 | (uint)type;
    }
    return entry;
}

Command* EmitCmdText<each T : IFormattable>(CommandType type, uint argc, StringId fmt, expand each T args) {
    int numSlots = 1;
    expand(numSlots += (each args).GetSize());

    Command* cmd = WriteCommand(type, argc + numSlots);
    if (!cmd) return nullptr;

    uint* argp = &cmd->Data[argc];
    *argp++ = fmt;
    expand(each args).WriteTo(argp);

    return cmd;
}

#define FIND_WIDGET(type, label, defaultret)                                 \
    if (havk__DebugToolsCtx == 0) return defaultret;                         \
    bool justAdded;                                                          \
    WidgetState* widget = FindWidget(type, getStringHash(label), justAdded); \
    if (!widget) return defaultret;

public void Text<each T : IFormattable>(string fmt, expand each T args) {
    if (havk__DebugToolsCtx == 0 || !IsEnabled) return;
    EmitCmdText(CommandType.UI_Text, 0, getStringHash(fmt), args);
}
public void Separator<each T : IFormattable>(string fmt, expand each T args) {
    if (havk__DebugToolsCtx == 0 || !IsEnabled) return;
    EmitCmdText(CommandType.UI_Separator, 0, getStringHash(fmt), args);
}
public bool Button(string label) {
    FIND_WIDGET(CommandType.UI_Button, label, false);
    return widget && widget->Params[6] == ctx->FrameCount;
}
public bool Checkbox(string label, bool defaultValue) {
    CheckboxEx(label, defaultValue);
    return defaultValue;
}
// Checkbox returning toggle change state.
public bool CheckboxEx(string label, inout bool value) {
    FIND_WIDGET(CommandType.UI_Checkbox, label, false);
    if (justAdded) widget->Params[0] = value ? 1 : 0;
    value = widget->Params[0] != 0;
    return widget->Params[6] == ctx->FrameCount;
}

public float Slider(string label, float defaultValue, 
                    float rangeMin = 0, float rangeMax = 1, 
                    string format = "%.2f") {
    FIND_WIDGET(CommandType.UI_Slider, label, defaultValue);
    if (justAdded) {
        widget->Params[0] = asuint(defaultValue);
        widget->Params[1] = asuint(rangeMin);
        widget->Params[2] = asuint(rangeMax);
        widget->Params[3] = getStringHash(format);
    }
    return asfloat(widget->Params[0]);
}
public vector<float, N> Drag<int N>(string label, vector<float, N> defaultValue, 
                                    float rangeMin = 0, float rangeMax = 1,
                                    string format = "%.2f") {
    FIND_WIDGET((CommandType)((int)CommandType.UI_Drag1 + N - 1), label, defaultValue);

    if (justAdded) {
        storeAligned<4>((vector<float, N>*)&widget->Params[0], defaultValue);
        widget->Params[4] = asuint(rangeMin);
        widget->Params[5] = asuint(rangeMax);
        widget->Params[6] = getStringHash(format);
    }
    return loadAligned<4>((vector<float, N>*)&widget->Params[0]);
}
public float4 ColorEdit(string label, float4 defaultValue) {
    FIND_WIDGET(CommandType.UI_ColorEdit, label, defaultValue);
    if (justAdded) storeAligned<4>((float4*)&widget->Params[0], defaultValue);
    return loadAligned<4>((float4*)&widget->Params[0]);
}
public float3 ColorEdit(string label, float3 defaultValue) {
    return ColorEdit(label, float4(defaultValue, -1)).rgb;
}
// Combo box. Option string is a list separated by semicolons, e.g. "OptionA;OptionB;OptionC"
public uint Combo(string label, uint defaultIndex, string options) {
    FIND_WIDGET(CommandType.UI_Combo, label, defaultIndex);
    if (justAdded) {
        widget->Params[0] = defaultIndex;
        widget->Params[1] = getStringHash(options);
    }
    return widget->Params[0];
}
public void Plot(string label, IFunc<float, float> sampler) {
    if (!IsEnabled) return;
    FIND_WIDGET(CommandType.UI_Plot, label, );
    if (justAdded) widget->Params[1] = 0;

    float* data = &ctx->PlotData[widget->Params[0]];
    uint numSamples = widget->Params[1];
    float rangeMin = asfloat(widget->Params[2]);
    float rangeMax = asfloat(widget->Params[3]);
    float scale = (rangeMax - rangeMin) / (numSamples - 1);

    for (uint i = 0; i < numSamples; i++) {
        data[i] = sampler(i * scale + rangeMin);
    }
}

public void PushID(uint id) {
    if (widgetStackedHash != 0) {
        FailAssert("Too many calls to PushID()", "ShaderDebugTools", 0);
    }
    widgetStackedHash = (id + 1) * 0x9E3779B1;
}
public void PopID() {
    if (widgetStackedHash == 0) {
        FailAssert("Unbalanced calls to PushID()/PopID()", "ShaderDebugTools", 0);
    }
    widgetStackedHash = 0;
}

// Default to wireframe
static uint g_FillColor = 0x00_000000;
static uint g_StrokeColor = 0xFF_FFFFFF;
static float g_StrokeWidth = 1.0f;

public void SetFill(uint color) {
    if (color != g_FillColor) {
        g_FillColor = color;
        EmitColor();
    }
}
public void SetFill(float r, float g, float b, float a = 1.0) { SetFill(float4(r, g, b, a)); }
public void SetFill(float3 color, float a = 1.0) { SetFill(float4(color, a)); }
public void SetFill(float4 color) { SetFill(packUnorm4x8(color.bgra)); }

public void SetStroke(float3 color, float width = 1.0f) { SetStroke(packUnorm4x8(float4(color.bgr, 1)), width); }
public void SetStroke(float4 color, float width = 1.0f) { SetStroke(packUnorm4x8(color.bgra), width); }
public void SetStroke(uint color, float width = 1.0f) {
    if (color != g_StrokeColor || width != g_StrokeWidth) {
        g_StrokeColor = color;
        g_StrokeWidth = width;
        EmitColor();
    }
}
public void NoFill() { SetFill(0); }
public void NoStroke() { SetStroke(0, 0); }

void EmitColor() {
    EMIT_CMD(G_SetColor, 3);
    cmd->Data[0] = g_FillColor;
    cmd->Data[1] = g_StrokeColor;
    cmd->Data[2] = asuint(g_StrokeWidth);
}
public void SetTransform(float3x3 transform) {
    EMIT_CMD(G_SetTransform, 3 * 3);
    storeAligned<4>((float3x3*)&cmd->Data[0], transform);
}
public void PushTransform() { EMIT_CMD(G_PushTransform, 0); }
public void PopTransform() { EMIT_CMD(G_PopTransform, 0); }
public void RotateX(float angleRad) { EMIT_CMD(G_RotateX, 1); cmd->Data[0] = asuint(angleRad); }
public void RotateY(float angleRad) { EMIT_CMD(G_RotateY, 1); cmd->Data[0] = asuint(angleRad); }
public void RotateZ(float angleRad) { EMIT_CMD(G_RotateZ, 1); cmd->Data[0] = asuint(angleRad); }
public void Translate(float3 offset) {
    EMIT_CMD(G_Translate, 3);
    storeAligned<4>((float3*)&cmd->Data[0], offset);
}
public void Scale(float3 factor) {
    EMIT_CMD(G_Scale, 3);
    storeAligned<4>((float3*)&cmd->Data[0], factor);
}

public void DrawLine3D(float3 p1, float3 p2) {
    EMIT_CMD(G_Line, 6);
    storeAligned<4>((float3*)&cmd->Data[0], p1);
    storeAligned<4>((float3*)&cmd->Data[3], p2);
}
public void DrawCube3D(float3 bbMin, float3 bbMax) {
    EMIT_CMD(G_Cube, 6);
    storeAligned<4>((float3*)&cmd->Data[0], bbMin);
    storeAligned<4>((float3*)&cmd->Data[3], bbMax);
}
public void DrawSphere3D(float3 center, float radius) {
    EMIT_CMD(G_Sphere, 4);
    storeAligned<4>((float4*)&cmd->Data[0], float4(center, radius));
}
public void DrawArrow3D(float3 origin, float3 direction) {
    EMIT_CMD(G_Arrow, 6);
    storeAligned<4>((float3*)&cmd->Data[0], origin);
    storeAligned<4>((float3*)&cmd->Data[3], direction);
}
public void DrawText3D<each T : IFormattable>(float3 pos, string fmt, expand each T args) {
    if (havk__DebugToolsCtx == 0 || !IsEnabled) return;
    Command* cmd = EmitCmdText(CommandType.G_Text, 3, getStringHash(fmt), args);
    if (!cmd) return;
    storeAligned<4>((float3*)&cmd->Data[0], pos);
}

static const float FLT_NAN = 0.0 / 0.0;

public void DrawLine2D(float2 p1, float2 p2) {
    DrawLine3D(float3(p1, FLT_NAN), float3(p2, FLT_NAN));
}
public void DrawRect2D(float2 p1, float2 p2) {
    DrawCube3D(float3(p1, FLT_NAN), float3(p2, FLT_NAN));
}
public void DrawCircle2D(float2 p1, float radius) {
    DrawSphere3D(float3(p1, FLT_NAN), radius);
}
public void DrawText2D<each T : IFormattable>(float2 pos, string fmt, expand each T args) {
    DrawText3D(float3(pos, FLT_NAN), fmt, args);
}

public interface IFormattable {
    uint GetSize();
    void WriteTo(inout uint* ptr);
};
extension<T : IArithmetic> T : IFormattable {
    uint GetSize() { return (sizeof(T) + 3) / 4; }
    void WriteTo(inout uint* ptr) { storeAligned<4>((This*)ptr, this); ptr = ptr + GetSize(); }
};
extension<T, int N> vector<T, N> : IFormattable {
    uint GetSize() { return (sizeof(T) * N + 3) / 4; }
    void WriteTo(inout uint* ptr) { storeAligned<4>((This*)ptr, this); ptr = ptr + GetSize(); }
};
extension string : IFormattable {
    uint GetSize() { return 1; }
    void WriteTo(inout uint* ptr) { *ptr++ = getStringHash(this); }
};

uint2 GetCurrentTID() {
    __stage_switch {
        case compute:
        case mesh:
        case amplification: {
            uint3 invocId = spirv_asm { result:$$uint3 = OpLoad builtin(GlobalInvocationId:uint3); };
            return invocId.xy;
        }
        case fragment: {
            float4 fragCoord = spirv_asm { result:$$float4 = OpLoad builtin(FragCoord:float4); };
            return uint2(fragCoord.xy);
        }
        case vertex: {
            uint vertexId = spirv_asm { result:$$int = OpLoad builtin(VertexIndex:int); };
            uint instanceId = spirv_asm { result:$$int = OpLoad builtin(InstanceIndex:int); };
            uint drawId = spirv_asm { result:$$int = OpLoad builtin(DrawIndex:int); };
            return uint2(vertexId, instanceId);
        }
        default:
            static_assert(false, "ThreadID is not defined in this shader stage.");
            return 0;
    }
}

public int3 GetPickedTID() {
    return ctx ? int3(ctx->SelectedTID, 0) : -1;
}
public void EnableIfPicked() {
    IsEnabled = havk__DebugToolsCtx != 0 && all(ctx->SelectedTID == GetCurrentTID());
}
public void EnableOnce() {
    IsEnabled = false;
    if (havk__DebugToolsCtx == 0) return;

    uint currId = ctx->EnableOncePID, prevId = havk__DebugToolsProgramId;
    if (currId != prevId) {
        InterlockedCompareExchange(ctx->EnableOncePID, currId, prevId, prevId);
    }
    IsEnabled = prevId != havk__DebugToolsProgramId;
}

public void PauseAfterFrame() {
    if (havk__DebugToolsCtx != 0) ctx->WantPauseAfterFrame = true;
}

public float GetTime() { return ctx ? ctx->Time : 0; }
public uint GetFrameCount() { return ctx ? ctx->FrameCount : 0; }
public float2 GetDisplaySize() { return ctx ? ctx->MainDisplaySize : 0; }

// Get last mouse pos at given state. 
public float2 GetMousePos(ButtonState state = ButtonState::None) { return ctx ? ctx->MousePos[(int)state] : 0; }
public bool IsMouseDown(MouseButton button = MouseButton.Left) { return IsKeyDown((Key)((int)Key.MouseLeft + (int)button)); }
public bool IsMouseClicked(MouseButton button = MouseButton.Left) { return IsKeyPressed((Key)((int)Key.MouseLeft + (int)button)); }
public bool IsMouseReleased(MouseButton button = MouseButton.Left) { return IsKeyReleased((Key)((int)Key.MouseLeft + (int)button)); }

public bool IsKeyDown(Key key) { return ctx && BitTest(&ctx->KeyDown[0], key - Key.NamedKey_BEGIN); }
public bool IsKeyPressed(Key key) { return ctx && BitTest(&ctx->KeyPressed[0], key - Key.NamedKey_BEGIN); }
public bool IsKeyReleased(Key key) { return ctx && BitTest(&ctx->KeyReleased[0], key - Key.NamedKey_BEGIN); }

static bool BitTest(uint* mask, uint index) {
    return (mask[index / 32u] >> (index % 32u) & 1) != 0;
}

public enum ButtonState : uint8_t {
    None,
    Down,      // Being held down
    Pressed,   // Just pressed (went from !down to down)
    Released,  // Just released (went from down to !down)
};

public enum MouseButton {
    Left = 0, Right = 1, Middle = 2
};
// Mirror of ImGuiKey
public enum Key {
    None = 0,
    NamedKey_BEGIN = 512,  // First valid key value (other than 0)

    Tab = 512,             // == NamedKey_BEGIN
    LeftArrow,
    RightArrow,
    UpArrow,
    DownArrow,
    PageUp,
    PageDown,
    Home,
    End,
    Insert,
    Delete,
    Backspace,
    Space,
    Enter,
    Escape,
    LeftCtrl, LeftShift, LeftAlt, LeftSuper,     // Also see ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt, ImGuiMod_Super below!
    RightCtrl, RightShift, RightAlt, RightSuper,
    Menu,
    Digit0, Digit1, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9,
    A, B, C, D, E, F, G, H, I, J,
    K, L, M, N, O, P, Q, R, S, T,
    U, V, W, X, Y, Z,
    F1, F2, F3, F4, F5, F6,
    F7, F8, F9, F10, F11, F12,
    F13, F14, F15, F16, F17, F18,
    F19, F20, F21, F22, F23, F24,
    Apostrophe,        // '
    Comma,             // ,
    Minus,             // -
    Period,            // .
    Slash,             // /
    Semicolon,         // ;
    Equal,             // =
    LeftBracket,       // [
    Backslash,         // \ (this text inhibit multiline comment caused by backslash)
    RightBracket,      // ]
    GraveAccent,       // `
    CapsLock,
    ScrollLock,
    NumLock,
    PrintScreen,
    Pause,
    Keypad0, Keypad1, Keypad2, Keypad3, Keypad4,
    Keypad5, Keypad6, Keypad7, Keypad8, Keypad9,
    KeypadDecimal,
    KeypadDivide,
    KeypadMultiply,
    KeypadSubtract,
    KeypadAdd,
    KeypadEnter,
    KeypadEqual,
    AppBack,               // Available on some keyboard/mouses. Often referred as "Browser Back"
    AppForward,
    Oem102,                // Non-US backslash.

    // Gamepad
    // (analog values are 0.0f to 1.0f)
    // (download controller mapping PNG/PSD at http://dearimgui.com/controls_sheets)
    //                     // XBOX        | SWITCH  | PLAYSTA. | -> ACTION
    GamepadStart,          // Menu        | +       | Options  |
    GamepadBack,           // View        | -       | Share    |
    GamepadFaceLeft,       // X           | Y       | Square   | Tap: Toggle Menu. Hold: Windowing mode (Focus/Move/Resize windows)
    GamepadFaceRight,      // B           | A       | Circle   | Cancel / Close / Exit
    GamepadFaceUp,         // Y           | X       | Triangle | Text Input / On-screen Keyboard
    GamepadFaceDown,       // A           | B       | Cross    | Activate / Open / Toggle / Tweak
    GamepadDpadLeft,       // D-pad Left  | "       | "        | Move / Tweak / Resize Window (in Windowing mode)
    GamepadDpadRight,      // D-pad Right | "       | "        | Move / Tweak / Resize Window (in Windowing mode)
    GamepadDpadUp,         // D-pad Up    | "       | "        | Move / Tweak / Resize Window (in Windowing mode)
    GamepadDpadDown,       // D-pad Down  | "       | "        | Move / Tweak / Resize Window (in Windowing mode)
    GamepadL1,             // L Bumper    | L       | L1       | Tweak Slower / Focus Previous (in Windowing mode)
    GamepadR1,             // R Bumper    | R       | R1       | Tweak Faster / Focus Next (in Windowing mode)
    GamepadL2,             // L Trigger   | ZL      | L2       | [Analog]
    GamepadR2,             // R Trigger   | ZR      | R2       | [Analog]
    GamepadL3,             // L Stick     | L3      | L3       |
    GamepadR3,             // R Stick     | R3      | R3       |
    GamepadLStickLeft,     //             |         |          | [Analog] Move Window (in Windowing mode)
    GamepadLStickRight,    //             |         |          | [Analog] Move Window (in Windowing mode)
    GamepadLStickUp,       //             |         |          | [Analog] Move Window (in Windowing mode)
    GamepadLStickDown,     //             |         |          | [Analog] Move Window (in Windowing mode)
    GamepadRStickLeft,     //             |         |          | [Analog]
    GamepadRStickRight,    //             |         |          | [Analog]
    GamepadRStickUp,       //             |         |          | [Analog]
    GamepadRStickDown,     //             |         |          | [Analog]

    // Aliases: Mouse Buttons (auto-submitted from AddMouseButtonEvent() calls)
    // - This is mirroring the data also written to io.MouseDown[], io.MouseWheel, in a format allowing them to be accessed via standard key API.
    MouseLeft, MouseRight, MouseMiddle, MouseX1, MouseX2, MouseWheelX, MouseWheelY,

    // [Internal] Reserved for mod storage
    ReservedForModCtrl, ReservedForModShift, ReservedForModAlt, ReservedForModSuper,

    // [Internal] If you need to iterate all keys (for e.g. an input mapper) you may use NamedKey_BEGIN..NamedKey_END.
    NamedKey_END,
    // NamedKey_COUNT = NamedKey_END - NamedKey_BEGIN,   slang fails here
    NamedKey_COUNT = 667 - 512,
};

};

// Rendering shaders
namespace dbg {

struct ShapeInstance {
    float3 Pos[2];
    uint32_t Color;
    uint32_t TransformIdx;
};
struct DrawParams {
    ShapeInstance* Objects;
    float3x4* Transforms;
    float3 ViewOrigin;
    float4x4 ProjMat;
};

[shader("vertex")]
void VS_DrawLine(
    uniform DrawParams pc,
    uint vertexId: SV_VulkanVertexID,
    out float4 clipPos: SV_Position,
    out float4 color)
{
    ShapeInstance obj = pc.Objects[vertexId >> 1];
    float3x4 transform = pc.Transforms[obj.TransformIdx];

    float3 vertPos = obj.Pos[vertexId & 1];
    vertPos = mul(transform, float4(vertPos, 1));

    clipPos = mul(pc.ProjMat, float4(vertPos, 1));
    color = unpackUnorm4x8ToFloat(obj.Color).bgra;
}
[shader("fragment")]
void FS_DrawLine(float4 color, out float4 outColor: SV_Target0, out float4 outNormal: SV_Target1) {
    outColor = color;
    outNormal = 1;
}

[shader("vertex")]
void VS_DrawCube(
    uniform DrawParams pc,
    uint vertexId: SV_VulkanVertexID,
    out float4 clipPos: SV_Position,
    out float4 color, out float3 vertPosRWS)
{
    // https://xcancel.com/SebAaltonen/status/1315982782439591938
    // Corner pos encoded in lower 3 bits of index
    // Only need positive planes with software culling trick: corner ^= asint(viewOrigin - bbMin);
    // (Not culling here because still want to render when camera is inside cube)
    // static const uint8_t kCubeFaceIndices[] = {
    //     0, 2, 1, 2, 3, 1,  // X+
    //     5, 4, 1, 1, 4, 0,  // Y+
    //     0, 4, 6, 0, 6, 2,  // Z+
    //     6, 5, 7, 6, 4, 5,  // X-
    //     2, 6, 3, 6, 7, 3,  // Y-
    //     7, 1, 3, 7, 5, 1,  // Z-
    // };
    ShapeInstance obj = pc.Objects[vertexId >> 3];
    float3x4 transform = pc.Transforms[obj.TransformIdx];

    int3 corner = vertexId << int3(31, 30, 29);
    float3 cornerPos = select(corner < 0, obj.Pos[0], obj.Pos[1]);
    float3 vertPos = mul(transform, float4(cornerPos, 1));

    clipPos = mul(pc.ProjMat, float4(vertPos, 1));
    color = unpackUnorm4x8ToFloat(obj.Color).bgra;
    vertPosRWS = vertPos - pc.ViewOrigin; // relative pos increases precision and avoids artifacts on deriv normals
}
[shader("fragment")]
void FS_DrawCube(float4 color, float3 vertPosRWS, out float4 outColor: SV_Target0, out float4 outNormal: SV_Target1) {
    outColor = color;
    outNormal.xyz = normalize(cross(ddx(vertPosRWS), -ddy(vertPosRWS))) * 0.5 + 0.5;
    outNormal.w = 1;
}

[shader("vertex")]
void VS_DrawArrow(
    uniform DrawParams pc,
    uint vertexId: SV_VulkanVertexID,
    uint instanceId: SV_VulkanInstanceID,
    out float4 clipPos: SV_Position,
    out float4 color, out float3 normal)
{
    ShapeInstance obj = pc.Objects[instanceId];
    float3x4 transform = pc.Transforms[obj.TransformIdx];

    const int subdiv = 12;
    const float headLength = 0.15, headRadius = 0.03, tailRadius = 0.01;
    float len = length(obj.Pos[1]);

    bool isHead = vertexId < subdiv * 3;
    uint turn = isHead ? vertexId : vertexId / 2u;
    float a = ((turn / 3u) + (turn % 3u)) * math::Tau / (subdiv - 1);
    float3 vertPos = float3(sin(a), cos(a), 0);
    normal = vertPos;

    if (isHead) { // cone with triangles
        bool isCenterPoint = (vertexId % 3u) == 2;
        vertPos.xy *= (isCenterPoint ? 0 : headRadius);
        vertPos.z = (isCenterPoint ? len : len - headLength);
        if (isCenterPoint) normal = float3(0, 0, 1);
    } else { // cylinder with quads
        vertPos.xy *= tailRadius;
        vertPos.z = (vertexId % 2u) * (len - headLength / 2);
    }
    float3x3 TBN = math::BuildOrthonormalBasis(normalize(obj.Pos[1]));
    vertPos = mul(transform, float4(mul(vertPos, TBN) + obj.Pos[0], 1));
    normal = mul(transform, float4(mul(normal, TBN), 0));

    clipPos = mul(pc.ProjMat, float4(vertPos, 1));
    color = unpackUnorm4x8ToFloat(obj.Color).bgra;
}
[shader("fragment")]
void FS_DrawArrow(float4 color, float3 normal, out float4 outColor: SV_Target0, out float4 outNormal: SV_Target1) {
    outColor = color;
    outNormal.xyz = normalize(normal) * 0.5 + 0.5;
    outNormal.w = 1;
}

// We'll render spheres through impostor cubes + rejection in fragment shader, because why not...
// https://bgolus.medium.com/rendering-a-sphere-on-a-quad-13c92025570c
[shader("vertex")]
void VS_DrawSphere(
    uniform DrawParams pc,
    uint vertexId: SV_VulkanVertexID,
    out float4 clipPos: SV_Position,
    out float4 color,
    out float3 rayOriginROS, out float3 rayDirROS,
    out nointerpolation float3 centerWS,
    out nointerpolation float radius,
    out nointerpolation uint transformIdx)
{
    ShapeInstance obj = pc.Objects[vertexId >> 3];
    float3x4 transform = pc.Transforms[obj.TransformIdx];
    float3 viewOriginOS = mul(math::Inverse(float3x3(transform)), pc.ViewOrigin - transform._m03_m13_m23);

    float3 center = obj.Pos[0];
    radius = obj.Pos[1].x;

    int3 corner = vertexId << int3(31, 30, 29);
    float3 cornerPos = center + select(corner < 0, -radius, +radius);
    float3 vertPos = mul(transform, float4(cornerPos, 1));

    clipPos = mul(pc.ProjMat, float4(vertPos, 1));
    color = unpackUnorm4x8ToFloat(obj.Color).bgra;

    centerWS = mul(transform, float4(center, 1));
    rayOriginROS = viewOriginOS - center;
    rayDirROS = cornerPos - viewOriginOS;

    transformIdx = obj.TransformIdx;
}

[shader("fragment")]
void FS_DrawSphere(
    uniform DrawParams pc,
    float4 color, float3 rayOriginROS, float3 rayDirROS, float3 centerWS, float radius, uint transformIdx,
    out float4 outColor: SV_Target0, out float4 outNormal: SV_Target1,
    out float outDepth : SV_Depth)
{
    rayDirROS = normalize(rayDirROS);
    float2 dist = RayIntersectSphere(rayOriginROS, rayDirROS, 0, radius);
    float hitDist = (dist.x < 0 ? dist.y : dist.x);
    if (dist.y < 0) discard;

    float3x4 transform = pc.Transforms[transformIdx];
    float3 hitPosRWS = mul(transform, float4(rayOriginROS + rayDirROS * hitDist, 1));
    float3 hitNormalWS = normalize(hitPosRWS) * (dist.x < 0 ? -1 : +1);

    float4 clipPos = mul(pc.ProjMat, float4(hitPosRWS + centerWS, 1));
    outDepth = clipPos.z / clipPos.w;
    outColor = color;
    outNormal.xyz = hitNormalWS * 0.5 + 0.5;
    outNormal.w = 1;
}

// Custom copy shader because vkCopyImageToBuffer doesn't support format conversion.
[numthreads(8, 8)]
void CS_CopyImageToRGBA8(
    uniform TextureHandle2D<float4> srcImage, uniform uint32_t* dstBuffer,
    uniform int4 srcRect, uniform uint dstStride,
    int2 pos: SV_DispatchThreadID)
{
    if (any(pos >= srcRect.zw)) return;
    bool inBounds = all(uint2(pos + srcRect.xy) < srcImage.Size);
    float4 value = inBounds ? srcImage.Load(pos + srcRect.xy) : 0;
    uint4 q = uint4(saturate(value) * 255.0 + 0.5);
    dstBuffer[pos.x + pos.y * dstStride] = q.r << 0 | q.g << 8 | q.b << 16 | q.a << 24;
}

// https://iquilezles.org/articles/intersectors
float2 RayIntersectSphere(float3 origin, float3 dir, float3 center, float radius) {
    float3 oc = origin - center;
    float b = dot(oc, dir);
    float c = dot(oc, oc) - radius * radius;
    float h = b * b - c;
    if (h < 0.0) return -1.0;
    h = sqrt(h);
    return float2(-b - h, -b + h);
}
float RayIntersectCapsule(float3 origin, float3 dir, float3 pa, float3 pb, float ra) {
    float3 ba = pb - pa;
    float3 oa = origin - pa;
    float baba = dot(ba, ba);
    float bard = dot(ba, dir);
    float baoa = dot(ba, oa);
    float rdoa = dot(dir, oa);
    float oaoa = dot(oa, oa);
    float a = baba - bard * bard;
    float b = baba * rdoa - baoa * bard;
    float c = baba * oaoa - baoa * baoa - ra * ra * baba;
    float h = b * b - a * c;
    if (h >= 0.0) {
        float t = (-b - sqrt(h)) / a;
        float y = baoa + t * bard;
        // body
        if (y > 0.0 && y < baba) return t;
        // caps
        float3 oc = (y <= 0.0) ? oa : origin - pb;
        b = dot(dir, oc);
        c = dot(oc, oc) - ra * ra;
        h = b * b - c;
        if (h > 0.0) return -b - sqrt(h);
    }
    return -1.0;
}

};