#include "ClaudeRunner.h"

#include <algorithm>
#include <mutex>
#include <thread>
#include <vector>

namespace {

std::mutex g_mutex;
HANDLE g_job = nullptr;
bool g_cancelRequested = false;

std::wstring ComSpec() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"ComSpec", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return buf;
    GetSystemDirectoryW(buf, MAX_PATH);
    return std::wstring(buf) + L"\\cmd.exe";
}

std::wstring LastError(const wchar_t* what) {
    DWORD code = GetLastError();
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring s = std::wstring(what) + L" failed (" + std::to_wstring(code) + L"): " + (msg ? msg : L"");
    if (msg) LocalFree(msg);
    return s;
}

void CloseIf(HANDLE& h) {
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
    h = nullptr;
}

}  // namespace

RunResult RunClaude(const std::wstring& commandLine, const std::wstring& workingDir,
                    const std::string& stdinUtf8) {
    RunResult r;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    HANDLE errFile = nullptr, job = nullptr;

    auto fail = [&](const wchar_t* what) {
        r.startError = LastError(what);
        CloseIf(inRead); CloseIf(inWrite); CloseIf(outRead); CloseIf(outWrite);
        CloseIf(errFile); CloseIf(job);
        return r;
    };

    if (!CreatePipe(&inRead, &inWrite, &sa, 0)) return fail(L"CreatePipe(stdin)");
    if (!CreatePipe(&outRead, &outWrite, &sa, 0)) return fail(L"CreatePipe(stdout)");
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    // stderr goes to a self-deleting temp file (avoids a second pipe and possible deadlocks)
    wchar_t tmpDir[MAX_PATH], tmpName[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmpDir) && GetTempFileNameW(tmpDir, L"ncc", 0, tmpName)) {
        errFile = CreateFileW(tmpName, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &sa, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    }
    if (!errFile || errFile == INVALID_HANDLE_VALUE) {
        errFile = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                              OPEN_EXISTING, 0, nullptr);
        if (errFile == INVALID_HANDLE_VALUE) return fail(L"CreateFile(stderr)");
    }

    // Job object: lets us kill claude + every child (node, tools) on cancel or shutdown
    job = CreateJobObjectW(nullptr, nullptr);
    if (!job) return fail(L"CreateJobObject");
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
    li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof(li));

    // Inherit exactly our three std handles and nothing else from Notepad++
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<char> attrBuf(attrSize);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize)) return fail(L"InitializeProcThreadAttributeList");
    HANDLE inheritList[3] = {inRead, outWrite, errFile};
    UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inheritList, sizeof(inheritList),
                              nullptr, nullptr);

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inRead;
    si.StartupInfo.hStdOutput = outWrite;
    si.StartupInfo.hStdError = errFile;
    si.lpAttributeList = attrs;

    const std::wstring comspec = ComSpec();
    // /s + outer quotes: cmd strips exactly the first and last quote, the rest is kept verbatim
    std::wstring cl = L"\"" + comspec + L"\" /d /s /c \"" + commandLine + L"\"";
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(comspec.c_str(), &cl[0], nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                                 EXTENDED_STARTUPINFO_PRESENT,
                             nullptr, workingDir.empty() ? nullptr : workingDir.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    if (!ok) return fail(L"CreateProcess");

    AssignProcessToJobObject(job, pi.hProcess);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_job = job;
        g_cancelRequested = false;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseIf(inRead);
    CloseIf(outWrite);

    // Feed stdin on its own thread so large prompts can never deadlock against stdout.
    HANDLE stdinHandle = inWrite;
    inWrite = nullptr;
    std::thread writer([stdinHandle, &stdinUtf8] {
        size_t off = 0;
        while (off < stdinUtf8.size()) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(stdinUtf8.size() - off, 64 * 1024));
            DWORD written = 0;
            if (!WriteFile(stdinHandle, stdinUtf8.data() + off, chunk, &written, nullptr)) break;
            off += written;
        }
        CloseHandle(stdinHandle);
    });

    HANDLE readerDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread reader([&] {
        char buf[16384];
        DWORD n = 0;
        while (ReadFile(outRead, buf, sizeof(buf), &n, nullptr) && n > 0) r.out.append(buf, n);
        SetEvent(readerDone);
    });

    WaitForSingleObject(pi.hProcess, INFINITE);
    // If some grandchild still holds stdout open, give it a moment, then kill the tree.
    if (WaitForSingleObject(readerDone, 3000) == WAIT_TIMEOUT) TerminateJobObject(job, 0);
    reader.join();
    writer.join();
    CloseHandle(readerDone);
    GetExitCodeProcess(pi.hProcess, &r.exitCode);
    CloseHandle(pi.hProcess);
    CloseIf(outRead);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        r.cancelled = g_cancelRequested;
        g_job = nullptr;
    }
    CloseIf(job);  // KILL_ON_JOB_CLOSE cleans up anything left behind

    if (SetFilePointer(errFile, 0, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(errFile, buf, sizeof(buf), &n, nullptr) && n > 0) r.err.append(buf, n);
    }
    CloseIf(errFile);
    return r;
}

void CancelClaude() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_job) {
        g_cancelRequested = true;
        TerminateJobObject(g_job, 1);
    }
}
