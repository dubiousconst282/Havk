#pragma once

#include <vector>
#include <memory>
#include <string>
#include <string_view>

// Miscellaneous OS-specific utilities that are intended for internal use.
namespace havx {

struct FileWatcher {
    FileWatcher(std::string_view path);
    ~FileWatcher();

    void PollChanges(std::vector<std::string>& changedFiles);
    
    // Blocks until changes are made or up to timeout. A negative value disables timeout.
    bool WaitForChanges(int timeoutMs);

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

struct JobProcess {
    JobProcess(std::string_view cmdLine, std::string_view workDir);
    ~JobProcess();

    // Read available data from stdout/stderr and append to buffer, without blocking.
    bool ReadStdout(std::string& buffer);

    bool HasExited(uint32_t* errorCode = nullptr);
private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
};

std::string GetExecFilePath();

// Thread sleep function accurate down to ~10 microseconds on most systems, but not guaranteed.
// https://blog.bearcats.nl/perfect-sleep-function
void PreciseSleep(double seconds);

// Returns high resolution timestamp, in seconds.
// QueryPerformanceCounter / clock_gettime(CLOCK_MONOTONIC).
double GetMonotonicTime();

#if _WIN32
// Windows-only bullshit to convert between UTF8 and UTF16 strings
// See https://utf8everywhere.org
std::wstring Win32_StringToWide(std::string_view str);
std::string Win32_WideToString(std::wstring_view wstr);
#endif

// Will return empty on failure.
std::vector<uint8_t> ReadFileBytes(std::string_view path);
bool WriteFileBytes(std::string_view path, const void* data, size_t size, bool overwrite);

};
