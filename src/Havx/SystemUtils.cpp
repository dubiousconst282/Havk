#include "SystemUtils.h"
#include <string>
#include <system_error>

#ifdef _WIN32
    #define WIN32_LEAN_AND_MEAN
    #define NOMINMAX
    #include <Windows.h>
#elif __linux__
    #include <unistd.h>

    #include <sys/inotify.h>
    #include <dirent.h>
    #include <poll.h>

    #include <fcntl.h>
    #include <sys/wait.h>
    #include <sys/prctl.h>

    #include <unordered_map>
#endif

namespace havx {

#ifdef _WIN32

std::wstring Win32_StringToWide(std::string_view str) {
    int len = MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), NULL, 0);
    auto wstr = std::wstring((size_t)len, '\0');
    MultiByteToWideChar(CP_UTF8, 0, str.data(), (int)str.size(), wstr.data(), len);
    return wstr;
}
std::string Win32_WideToString(std::wstring_view wstr) {
    int len = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    auto str = std::string((size_t)len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), str.data(), len, NULL, NULL);
    return str;
}
#endif

struct TerminatedStrView {
    const char* ptr;
    std::string copy;

    TerminatedStrView(std::string_view view) : ptr(view.data()) {
        // Do not deref if crossing pages, otherwise might AV
        const char* end = view.data() + view.size();
        if (((uintptr_t)end & 4095) == 0 || *end != '\0') {
            copy = std::string(view);
            ptr = copy.data();
        }
    }
};

#ifdef _WIN32

struct FileWatcher::Impl {
    HANDLE _dirHandle;
    OVERLAPPED _overlapped = {};
    uint8_t _eventBuffer[4096];

    Impl(std::string_view baseDir) {
        _dirHandle = CreateFileW(Win32_StringToWide(baseDir).c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                  OPEN_EXISTING,
                                  FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
        _overlapped.hEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        ReadChangesAsync();
    };
    ~Impl() {
        CloseHandle(_dirHandle);
        CloseHandle(_overlapped.hEvent);
    }
    void ReadChangesAsync() {
        ReadDirectoryChangesW(_dirHandle, _eventBuffer, sizeof(_eventBuffer), true, FILE_NOTIFY_CHANGE_LAST_WRITE,
                              NULL, &_overlapped, NULL);
    }
};

void FileWatcher::PollChanges(std::vector<std::string>& changedFiles) {
    DWORD numBytesReceived;

    while (GetOverlappedResult(_impl->_dirHandle, &_impl->_overlapped, &numBytesReceived, false)) {
        uint8_t* eventPtr = _impl->_eventBuffer;
        while (true) {
            auto event = (FILE_NOTIFY_INFORMATION*)eventPtr;
            auto fileName = Win32_WideToString(std::wstring_view(event->FileName, event->FileNameLength / 2));

            if (event->Action == FILE_ACTION_MODIFIED &&
                // Some apps (VSCode) will cause multiple events to be fired for the same file.
                (changedFiles.size() == 0 || changedFiles[changedFiles.size() - 1] != fileName)) {
                changedFiles.push_back(fileName);
            }
            if (event->NextEntryOffset == 0) {
                break;
            }
            eventPtr += event->NextEntryOffset;
        }
        _impl->ReadChangesAsync();
    }
}
bool FileWatcher::WaitForChanges(int timeoutMs) {
    return WaitForSingleObject(_impl->_overlapped.hEvent, timeoutMs < 0 ? INFINITE : (DWORD)timeoutMs) == WAIT_OBJECT_0;
}
#elif __linux__

struct FileWatcher::Impl {
    int _fd;
    std::unordered_map<int, std::string> _subdirs;
    std::string _baseDir;

    Impl(std::string_view baseDir) {
        _baseDir = baseDir;
        if (_baseDir.ends_with('/')) _baseDir.resize(_baseDir.size() - 1);

        _fd = inotify_init1(IN_NONBLOCK);

        if (!AddSubDirectory("")) {
            throw std::system_error(errno, std::generic_category(), "Failed to setup inotify");
        }
    }

    bool AddSubDirectory(const std::string& subpath) {
        std::string fullPath = _baseDir + subpath;

        int wd = inotify_add_watch(_fd, fullPath.c_str(), IN_MODIFY);
        if (wd < 0) return false;

        _subdirs.insert_or_assign(wd, subpath);

        DIR* dir = opendir(fullPath.c_str());
        if (!dir) return false;

        while (dirent* entry = readdir(dir)) {
            if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
                if (!AddSubDirectory(subpath + '/' + entry->d_name)) break;
            }
        }
        closedir(dir);
        return true;
    }

    ~Impl() {
        close(_fd);
    }
};

void FileWatcher::PollChanges(std::vector<std::string>& changedFiles) {
    char buffer[4096];
    ssize_t len;

    while ((len = read(_impl->_fd, buffer, sizeof(buffer))) > 0) {
        for (char* ptr = buffer; ptr < buffer + len; ) {
            auto event = (inotify_event*)ptr;
            auto fileName = _impl->_baseDir + _impl->_subdirs[event->wd] + "/" + event->name;

            if ((event->mask & IN_MODIFY) &&
                // Some apps (VSCode) will cause multiple events to be fired for the same file.
                (changedFiles.size() == 0 || changedFiles[changedFiles.size() - 1] != fileName)) {
                changedFiles.push_back(fileName);
            }
            ptr += sizeof(inotify_event) + event->len;
        }
    }
}

bool FileWatcher::WaitForChanges(int timeoutMs) {
    pollfd pfd = { .fd = _impl->_fd, .events = POLLIN };
    return poll(&pfd, 1, timeoutMs) > 0;
}

#else

#warning "FileWatcher is not implemented for this platform"

struct FileWatcher::Impl {
    Impl(std::string_view baseDir) { }
};
void FileWatcher::PollChanges(std::vector<std::filesystem::path>& changedFiles) {}

bool FileWatcher::WaitForChanges(int timeoutMs) {
    // Dummy sleep to prevent busy waiting.
    PreciseSleep(timeoutMs / 1000.0);
    return false;
}

#endif

FileWatcher::FileWatcher(std::string_view baseDir) {
    _impl = std::make_unique<Impl>(baseDir);
};
FileWatcher::~FileWatcher() = default;



#ifdef _WIN32
// https://blog.s-schoener.com/2024-06-16-stream-redirection-win32/

struct JobProcess::Impl {
    HANDLE hJob;
    HANDLE hProcess;
    HANDLE hPipeStdout;
    OVERLAPPED overlapped;
    char readBufferStdout[4096];
};

static void CreateAsyncPipe(HANDLE* outRead, HANDLE* outWrite) {
    char pipeName[64];
    snprintf(pipeName, sizeof(pipeName), "\\\\.\\pipe\\havk_shader_reload_watcher_%lu", GetCurrentThreadId());

    HANDLE read = CreateNamedPipeA(pipeName, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1,
                                   0, 4096, 0, nullptr);
    if (read == INVALID_HANDLE_VALUE) {
        throw std::system_error((int)GetLastError(), std::system_category(), "Failed to create named pipe");
    }

    // Now create a handle for the other end of the pipe. We are going to pass that handle to the
    // process we are creating, so we need to specify that the handle can be inherited.
    // Also note that we are NOT setting FILE_FLAG_OVERLAPPED. We could set it, but that's not relevant
    // for our end of the pipe. (We do not expect async writes.)
    SECURITY_ATTRIBUTES saAttr = { .nLength = sizeof(SECURITY_ATTRIBUTES), .bInheritHandle = TRUE };
    HANDLE write = CreateFileA(pipeName, GENERIC_WRITE, 0, &saAttr, OPEN_EXISTING, 0, 0);

    if (write == INVALID_HANDLE_VALUE) {
        throw std::system_error((int)GetLastError(), std::system_category(), "Failed to create named pipe");
    }
    *outRead = read;
    *outWrite = write;
}

JobProcess::JobProcess(std::string_view cmdLine, std::string_view workDir) {
    // Setup pipe to redirect stdout and stderr.
    HANDLE stdOutRead, stdOutWrite;
    CreateAsyncPipe(&stdOutRead, &stdOutWrite);

    STARTUPINFOW startupInfo = {
        .cb = sizeof(STARTUPINFOW),
        .dwFlags = STARTF_USESTDHANDLES,
        .hStdOutput = stdOutWrite,
        .hStdError = stdOutWrite,
    };
    PROCESS_INFORMATION procInfo = {};

    BOOL success = CreateProcessW(nullptr, Win32_StringToWide(cmdLine).data(), nullptr, nullptr, TRUE, 0, nullptr,
                                  Win32_StringToWide(workDir).data(), &startupInfo, &procInfo);
    if (!success) {
        throw std::system_error((int)GetLastError(), std::system_category(), "Failed to create process");
    }

    // Now we can must close the write-handles for our pipes. The write handle has been inherited by the
    // subprocess, and if we don't close these handles the writing end of the pipe is staying open indefinitely.
    // Then our read-calls would keep waiting even when the child process has already exited.
    CloseHandle(stdOutWrite);

    // Avoid leaking the thread handle.
    CloseHandle(procInfo.hThread);

    _impl = std::make_unique<Impl>();

    _impl->hJob = CreateJobObject(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimitInfo = {};
    jobLimitInfo.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(_impl->hJob, JobObjectExtendedLimitInformation, &jobLimitInfo, sizeof(jobLimitInfo));

    if (!AssignProcessToJobObject(_impl->hJob, procInfo.hProcess)) {
        TerminateProcess(_impl->hProcess, EXIT_FAILURE);
        throw std::system_error((int)GetLastError(), std::system_category(), "Failed to assign job to process");
    }

    _impl->hProcess = procInfo.hProcess;
    _impl->hPipeStdout = stdOutRead;
    _impl->overlapped = {};

    // Request first async read to our internal buffer
    ReadFile(_impl->hPipeStdout, _impl->readBufferStdout, sizeof(_impl->readBufferStdout), nullptr, &_impl->overlapped);
}
JobProcess::~JobProcess() {
    CloseHandle(_impl->hProcess);
    CloseHandle(_impl->hPipeStdout);
    CloseHandle(_impl->hJob);
}

bool JobProcess::ReadStdout(std::string& buffer) {
    DWORD numBytesRead;
    if (!GetOverlappedResult(_impl->hPipeStdout, &_impl->overlapped, &numBytesRead, false)) {
        return false;
    }
    buffer.append(_impl->readBufferStdout, numBytesRead);
    ReadFile(_impl->hPipeStdout, _impl->readBufferStdout, sizeof(_impl->readBufferStdout), nullptr, &_impl->overlapped);
    return true;
}
bool JobProcess::HasExited(uint32_t* exitCode) {
    DWORD ec;
    GetExitCodeProcess(_impl->hProcess, &ec);

    if (exitCode != nullptr) *exitCode = ec;
    return ec != STILL_ACTIVE;
}

#elif __linux__

struct JobProcess::Impl {
    pid_t pid;
    int fdStdout;
};

JobProcess::JobProcess(std::string_view cmdLine, std::string_view workDir) {
    int pfd[2];
    if (pipe(pfd) < 0) {
        throw std::system_error(errno, std::generic_category(), "Failed to create process pipes");
    }

    pid_t parent_pid = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pfd[1], STDOUT_FILENO);

        close(pfd[0]);
        close(pfd[1]);

        int r = prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (r < 0 || getppid() != parent_pid) exit(0);

        chdir(TerminatedStrView(workDir).ptr);
        execl("/bin/sh", "sh", "-c", TerminatedStrView(cmdLine).ptr, nullptr);
        exit(1);
    } else if (pid < 0) {
        throw std::system_error(errno, std::generic_category(), "Failed to create process");
    }
    close(pfd[1]);
    fcntl(pfd[0], F_SETFL, fcntl(pfd[0], F_GETFL) | O_NONBLOCK);

    _impl = std::make_unique<Impl>();
    _impl->pid = pid;
    _impl->fdStdout = pfd[0];
}
JobProcess::~JobProcess() {
    close(_impl->fdStdout);
}

bool JobProcess::ReadStdout(std::string& buffer) {
    size_t origSize = buffer.size();
    buffer.resize(origSize + 4096);

    ssize_t numBytesRead = read(_impl->fdStdout, buffer.data() + origSize, 4096);

    buffer.resize(origSize + (numBytesRead < 0 ? 0 : (size_t)numBytesRead));
    return numBytesRead > 0;
}
bool JobProcess::HasExited(uint32_t* exitCode) {
    int status;
    pid_t res = waitpid(_impl->pid, &status, WNOHANG);
    if (exitCode != nullptr) *exitCode = WEXITSTATUS(status);
    return res != 0;
}

#else

#warning "JobProcess is not implemented for this platform"

struct JobProcess::Impl {
};

JobProcess::JobProcess(std::string_view cmdLine, std::string_view workDir) {
}
JobProcess::~JobProcess() {
}

bool JobProcess::ReadStdout(std::string& buffer) {
    return false;
}
bool JobProcess::HasExited(uint32_t* exitCode) {
    return true;
}

#endif


std::string GetExecFilePath() {
    // https://stackoverflow.com/a/55579815
#ifdef _WIN32
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(NULL, path, MAX_PATH);
    return Win32_WideToString(std::wstring_view(path, len));
#else
    char result[PATH_MAX];
    ssize_t count = readlink("/proc/self/exe", result, PATH_MAX);
    return std::string(result, (count > 0) ? (size_t)count : 0);
#endif
}

// https://blog.bearcats.nl/perfect-sleep-function
void PreciseSleep(double seconds) {
#if _WIN32
    static HANDLE timer;
    static INT64 qpcPerSecond;

    if (qpcPerSecond == 0) {
        timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

        LARGE_INTEGER qpf;
        QueryPerformanceFrequency(&qpf);
        qpcPerSecond = qpf.QuadPart;
    }
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    INT64 targetQpc = (INT64)(qpc.QuadPart + seconds * qpcPerSecond);

    // Try using a high resolution timer first, fallback to sleep if unavailable.
    if (timer) {
        const double TOLERANCE = 0.001'02;
        INT64 maxTicks = 9'500;
        for (;;) {
            double remainingSeconds = (targetQpc - qpc.QuadPart) / (double)qpcPerSecond;
            INT64 sleepTicks = (INT64)((remainingSeconds - TOLERANCE) * 10'000'000);
            if (sleepTicks <= 0) break;

            LARGE_INTEGER due;
            due.QuadPart = -(sleepTicks > maxTicks ? maxTicks : sleepTicks);
            SetWaitableTimerEx(timer, &due, 0, NULL, NULL, NULL, 0);
            WaitForSingleObject(timer, INFINITE);
            QueryPerformanceCounter(&qpc);
        }
    } else {
        const double TOLERANCE = 0.000'02;
        const int SchedulerPeriodMs = 15;
        double sleepMs = (seconds - TOLERANCE) * 1000 - SchedulerPeriodMs;  // Sleep for 1 scheduler period less than requested.
        int sleepSlices = (int)(sleepMs / SchedulerPeriodMs);
        if (sleepSlices > 0) Sleep((DWORD)sleepSlices * SchedulerPeriodMs);
        QueryPerformanceCounter(&qpc);
    }

    // Spin for any remaining time
    while (qpc.QuadPart < targetQpc) {
        YieldProcessor();
        QueryPerformanceCounter(&qpc);
    }
#else
    constexpr int64_t onesec_ns = 1e9;
    const int64_t tolerance = 80'000;

    timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    int64_t now = tp.tv_sec * onesec_ns + tp.tv_nsec;
    int64_t target = now + int64_t(seconds * onesec_ns + 0.5) - tolerance;

    timespec targetspec = { .tv_sec = target / onesec_ns, .tv_nsec = target % onesec_ns };
    while (target > now && clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &targetspec, NULL) < 0 && errno == EINTR) {
        ;
    }

    while (now < target + tolerance) {
#if __SSE2__ || __riscv
    __asm__ volatile("pause");
#elif __ARM_ARCH
    __asm__ volatile("yield");
#else
    #warning "Missing PAUSE"
#endif
        clock_gettime(CLOCK_MONOTONIC, &tp);
        now = tp.tv_sec * onesec_ns + tp.tv_nsec;
    }
#endif
}

double GetMonotonicTime() {
#if _WIN32
    static double freq;

    LARGE_INTEGER qpc;
    if (freq == 0) {
        QueryPerformanceFrequency(&qpc);
        freq = 1.0 / qpc.QuadPart;
    }
    QueryPerformanceCounter(&qpc);
    return qpc.QuadPart * freq;
#else
    timespec tp;
    clock_gettime(CLOCK_MONOTONIC, &tp);
    return (double)tp.tv_sec + (double)tp.tv_nsec * 1E-9;
#endif
}

std::vector<uint8_t> ReadFileBytes(std::string_view path) {
    FILE* fs;
#if _WIN32
    _wfopen_s(&fs, Win32_StringToWide(path).c_str(), L"rb");
#else
    fs = fopen(std::string(path).c_str(), "rb");  // must copy to ensure null terminator
#endif
    if (!fs) return {};

    fseek(fs, 0, SEEK_END);
    auto buffer = std::vector<uint8_t>((size_t)ftell(fs));
    fseek(fs, 0, SEEK_SET);
    fread(buffer.data(), 1, buffer.size(), fs);
    fclose(fs);

    return buffer;
}
bool WriteFileBytes(std::string_view path, const void* data, size_t size, bool overwrite) {
    FILE* fs;
#if _WIN32
    _wfopen_s(&fs, Win32_StringToWide(path).c_str(), overwrite ? L"wb" : L"wbx");
#else
    fs = fopen(std::string(path).c_str(), overwrite ? "wb" : "wbx");  // must copy to ensure null terminator
#endif
    if (!fs) return false;

    fwrite(data, 1, size, fs);
    fclose(fs);

    return true;
}

};  // namespace havx