#include "shader_lab/lab.hpp"
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
namespace sl {
#ifdef _WIN32
namespace {
struct Handle {
    HANDLE h = nullptr;
    ~Handle() {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    }
    operator HANDLE() const {
        return h;
    }
};
std::wstring quote(const std::wstring &s) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (auto c : s) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'\"')
            out.append(slashes * 2 + 1, L'\\');
        else
            out.append(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    out += L'\"';
    return out;
}
} // namespace
#endif
ProcessResult process(const fs::path &executable, const std::vector<std::string> &args,
                      const fs::path &cwd, const fs::path &log, std::chrono::milliseconds timeout) {
    const auto begin = std::chrono::steady_clock::now();
    ProcessResult result;
    result.working_directory = fs::absolute(cwd).lexically_normal();
    if (timeout.count() <= 0 || timeout.count() > 86400000)
        throw std::runtime_error("timeout must be 1..86400000 ms");
    fs::create_directories(cwd);
    fs::create_directories(log.parent_path());
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    Handle output{CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr)};
    Handle input{CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, 0, nullptr)};
    Handle job{CreateJobObjectW(nullptr, nullptr)};
    if (output.h == INVALID_HANDLE_VALUE || input.h == INVALID_HANDLE_VALUE || !job.h)
        throw std::runtime_error("worker handle creation failed");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_PROCESS_MEMORY;
    limits.ProcessMemoryLimit = 2ull * 1024 * 1024 * 1024;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        throw std::runtime_error("worker job limits failed");
    std::wstring command = quote(executable.wstring());
    for (const auto &arg : args)
        command += L" " + quote(path_from(arg).wstring());
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = input;
    si.StartupInfo.hStdOutput = output;
    si.StartupInfo.hStdError = output;
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<uint8_t> attrs(size);
    si.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &size))
        throw std::runtime_error("worker attributes failed");
    struct Attr {
        LPPROC_THREAD_ATTRIBUTE_LIST p;
        ~Attr() {
            DeleteProcThreadAttributeList(p);
        }
    } attr{si.lpAttributeList};
    HANDLE handles[] = {input.h, output.h};
    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles, sizeof(handles), nullptr, nullptr))
        throw std::runtime_error("worker handle isolation failed");
    PROCESS_INFORMATION pi{};
    // Win32 file APIs can use long paths, but CreateProcessW's working directory
    // still has a MAX_PATH limit. Requests/artifact paths are absolute; launch
    // from the nearest existing short ancestor and disclose the actual cwd.
    while (result.working_directory.native().size() >= MAX_PATH - 1) {
        auto parent = result.working_directory.parent_path();
        if (parent == result.working_directory || parent.empty())
            throw std::runtime_error("cannot find a Windows worker launch directory within MAX_PATH");
        result.working_directory = std::move(parent);
    }
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                        result.working_directory.c_str(), &si.StartupInfo, &pi))
        throw std::runtime_error("cannot launch worker; Win32=" + std::to_string(GetLastError()));
    Handle proc{pi.hProcess}, thread{pi.hThread};
    if (!AssignProcessToJobObject(job, proc)) {
        TerminateProcess(proc, 125);
        WaitForSingleObject(proc, INFINITE);
        throw std::runtime_error("cannot isolate worker in job");
    }
    ResumeThread(thread);
    auto wait = WaitForSingleObject(proc, DWORD(timeout.count()));
    if (wait == WAIT_TIMEOUT) {
        result.timed_out = true;
        TerminateJobObject(job, 124);
        WaitForSingleObject(proc, INFINITE);
    } else if (wait != WAIT_OBJECT_0) {
        TerminateJobObject(job, 125);
        throw std::runtime_error("worker wait failed");
    }
    DWORD code = 0;
    GetExitCodeProcess(proc, &code);
    result.exit_code = code;
#else
    // No shell and no guest executable launch. The child executes only the selected compiler
    // worker.
    std::vector<std::string> owned{path_text(executable)};
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (auto &s : owned)
        argv.push_back(s.data());
    argv.push_back(nullptr);
    int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        throw std::runtime_error("cannot open worker log");
    pid_t pid = fork();
    if (pid < 0) {
        close(fd);
        throw std::runtime_error("fork failed");
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        int nullfd = open("/dev/null", O_RDONLY);
        dup2(nullfd, STDIN_FILENO);
        close(nullfd);
        if (chdir(cwd.c_str()))
            _exit(125);
        rlimit memory{2ull * 1024 * 1024 * 1024, 2ull * 1024 * 1024 * 1024};
        setrlimit(RLIMIT_AS, &memory);
        execv(executable.c_str(), argv.data());
        _exit(127);
    }
    close(fd);
    setpgid(pid, pid);
    int status = 0;
    while (true) {
        auto r = waitpid(pid, &status, WNOHANG);
        if (r == pid)
            break;
        if (r < 0) {
            kill(-pid, SIGKILL);
            throw std::runtime_error("waitpid failed");
        }
        if (std::chrono::steady_clock::now() - begin >= timeout) {
            result.timed_out = true;
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(-pid, SIGKILL);
    result.exit_code =
        WIFEXITED(status) ? uint32_t(WEXITSTATUS(status)) : uint32_t(128 + WTERMSIG(status));
#endif
    result.elapsed_ms = uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - begin)
                                     .count());
    return result;
}
} // namespace sl
