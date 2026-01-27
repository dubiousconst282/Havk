#pragma once

#include "Havk.h"
#include "Havx/SystemUtils.h"

#include <functional>
#include <unordered_map>
#include <filesystem>

namespace havk {

struct ReloadWatcher {
    using ReloadCallback = std::function<void(Span<const ModuleDesc> modules)>;

    ReloadWatcher(std::string_view srcBaseDir, std::string_view fullWatcherCmd) {
        _srcBaseDir = srcBaseDir;
        _watcherProcess = std::make_unique<havx::JobProcess>(fullWatcherCmd, srcBaseDir);
    }

    void BeginTracking(Pipeline* pipe, Span<const ModuleDesc> modules, ReloadCallback&& cb);
    void StopTracking(Pipeline* pipe);

    void Poll(DeviceContext* ctx);

    // Attempts to create watcher for currently executing app, returning null on failure.
    // This requires a `.shaderwatch` file to exist next the running application's binary,
    // for example: `build/FancyApp.exe` `build/FancyApp.exe.shaderwatch`
    static std::unique_ptr<ReloadWatcher> TryCreateForCurrentApp();

private:
    struct PipelineRebuildInfo {
        std::string ModulePaths;  // String list separated by '\0'
        ReloadCallback ReloadCb;
    };
    std::unordered_map<Pipeline*, PipelineRebuildInfo> _pipelines;
    std::unique_ptr<havx::JobProcess> _watcherProcess;
    std::string _srcBaseDir;

    std::string _stdoutBuffer;
};

static bool ReadLine(std::string_view& buffer, std::string_view& line) {
    while (true) {
        size_t endPos = buffer.find('\n');
        if (endPos == std::string::npos) break;

        line = buffer.substr(0, endPos);
        buffer.remove_prefix(endPos + 1);

        if (line.ends_with('\r')) line.remove_suffix(1);
        if (!line.empty()) return true;
    }
    return false;
}

inline std::unique_ptr<ReloadWatcher> ReloadWatcher::TryCreateForCurrentApp() {
    auto metaData = havx::ReadFileBytes(havx::GetExecFilePath() + ".shaderwatch");
    if (metaData.empty()) return nullptr;

    auto metaText = std::string_view((const char*)metaData.data(), metaData.size());
    std::string watcherCmd, baseDir;

    for (std::string_view line; ReadLine(metaText, line);) {
        auto key = line.substr(0, line.find_first_of(':'));
        auto val = line.substr(key.size() + 2);

        if (key == "watcher_cmd") {
            watcherCmd = val;
        } else if (key == "base_dir") {
            baseDir = val;
        }
    }

    if (watcherCmd.empty() || baseDir.empty()) return nullptr;
    return std::make_unique<ReloadWatcher>(baseDir, watcherCmd);
}

inline void ReloadWatcher::BeginTracking(Pipeline* pipe, Span<const ModuleDesc> modules, ReloadCallback&& cb) {
    std::string modPaths = "";

    for (auto& mod : modules) {
        if (mod.SourcePath == nullptr) return;

        modPaths.append(mod.SourcePath).append(1, '\0');
        modPaths.append(mod.EntryPoint).append(1, '\0');
    }
    _pipelines.insert({ pipe, { .ModulePaths = modPaths, .ReloadCb = std::move(cb) } });
}
inline void ReloadWatcher::StopTracking(Pipeline* pipe) { _pipelines.erase(pipe); }

inline void ReloadWatcher::Poll(DeviceContext* ctx) {
    if (_watcherProcess == nullptr) return;

    if (_watcherProcess->HasExited()) {
        // TODO: restart shader watcher server on crash
        ctx->Log(LogLevel::Error, "[ShaderReload] Watcher server has exited unexpectedly.");
        _watcherProcess = nullptr;
        return;
    }
    _watcherProcess->ReadStdout(_stdoutBuffer);
    std::string_view buffer = _stdoutBuffer;

    for (std::string_view line; ReadLine(buffer, line); ) {
        if (!line.starts_with("Recompiled ")) {
            ctx->Log(LogLevel::Info, "[ShaderReload] %.*s", (int)line.size(), line.data());
            continue;
        }
        line.remove_prefix(strlen("Recompiled "));

        size_t splitPos = line.find(" -> ");
        auto relSourcePath = std::filesystem::relative(std::u8string_view{ (char8_t*)line.data(), splitPos }, _srcBaseDir);
        auto spirvData = havx::ReadFileBytes(line.substr(splitPos + 4));

        if (spirvData.empty()) {
            // We'll occasionally be too late for a reload cycle and fail to open the
            // SPIR-V file because it is being rewriten with contents of a newer change.
            ctx->Log(LogLevel::Warn, "[ShaderReload] Could not reload '%s'. (Failed to read SPIR-V file. Try again?)", relSourcePath.filename().string().c_str());
            continue;
        }

        for (auto& [pipe, info] : _pipelines) {
            std::vector<ModuleDesc> modules;
            std::string& srcPaths = info.ModulePaths;

            for (size_t pos = 0; pos < srcPaths.size();) {
                size_t sepPos = srcPaths.find('\0', pos) + 1;
                const char* sourcePath = srcPaths.data() + pos;
                const char* entryPoint = srcPaths.data() + sepPos;

                if (relSourcePath.compare(sourcePath) != 0) {
                    assert(modules.empty());
                    break;
                }

                assert((uintptr_t)spirvData.data() % 4 == 0);
                modules.push_back({
                    .Code = (const uint32_t*)spirvData.data(),
                    .CodeSize = (uint32_t)spirvData.size(),
                    .Flags = ModuleDesc::kNoReload,
                    .EntryPoint = entryPoint,
                    .SourcePath = sourcePath,
                });
                pos = srcPaths.find('\0', sepPos) + 1;
            }
            if (!modules.empty()) {
                info.ReloadCb(modules);
            }
        }
    }
    _stdoutBuffer.erase(0, (size_t)(buffer.data() - _stdoutBuffer.data()));
}

};  // namespace havk