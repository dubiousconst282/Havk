#pragma once
#include <Havk/Havk.h>

// Very basic CPU+GPU frame profiler.
// 
// Current limitations:
// - Only one call per scope is supported
// - Not thread safe
//
// GPU performance counters are also shown if VK_KHR_performance_query is available.
// As of mid 2025, this ext is only implemented on Intel and AMD (Mesa RADV) drivers.
// For Intel on Linux, `sudo sysctl -w dev.i915.perf_stream_paranoid=0` must be set to enable support.
namespace havx::PerfMon {

struct ScopeHandle {
    void* InternalData = nullptr;
    ~ScopeHandle();

    ScopeHandle& SetText(const char* fmt, ...);
};

// Bind CommandList and DeviceContext from where profiling will happen.
// This function marks frame boundaries, and must not be called again until the previously bound list has been submitted.
void NewFrame(havk::CommandList& list);

// Call this before destroying any previously bound DeviceContext.
void Shutdown();

// This function should not be called directly, but wrapped in a macro so that calls can be
// easily stripped out in release builds. See also, `HAVK_PERFMON_OVERRIDE_TRACY_MACROS`.
ScopeHandle BeginScope(const char* label, uint32_t color = 0);

// Draw UI using ImGui. Must only be called after all Begin()/End() calls.
void DrawFrame();

};  // namespace havx::PerfMon

#if defined(_MSC_VER) && !defined(__clang__)
    #define HAVK_PERFMON_FILE_NAME __FILE__
#else
    #define HAVK_PERFMON_FILE_NAME __FILE_NAME__
#endif

#ifdef HAVK_PERFMON_OVERRIDE_TRACY_MACROS
    #define HAVK_PERFMON_STRINGIFY0(x) #x
    #define HAVK_PERFMON_STRINGIFY(x) HAVK_PERFMON_STRINGIFY0(x)

    #undef ZoneScoped
    #undef ZoneScopedN
    #undef ZoneScopedNC
    #define ZoneScopedNC(name, color) auto __havk_perf_scope = havx::PerfMon::BeginScope(name, color)
    #define ZoneScopedN(name) ZoneScopedNC(name, 0)
    #define ZoneScoped ZoneScopedN(HAVK_PERFMON_FILE_NAME ":" HAVK_PERFMON_STRINGIFY(__LINE__))
#endif