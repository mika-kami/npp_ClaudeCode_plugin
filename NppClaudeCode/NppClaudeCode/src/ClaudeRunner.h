#pragma once
#include <windows.h>
#include <string>

struct RunResult {
    bool cancelled = false;
    DWORD exitCode = 0;
    std::string out;          // stdout (UTF-8)
    std::string err;          // stderr (UTF-8)
    std::wstring startError;  // non-empty if the process could not be started
};

// Runs `commandLine` through cmd.exe (so claude.cmd / claude.exe on PATH both work),
// writes `stdinUtf8` to its stdin and collects stdout/stderr. Blocking: call from a worker thread.
RunResult RunClaude(const std::wstring& commandLine, const std::wstring& workingDir,
                    const std::string& stdinUtf8);

// Kills the currently running Claude process tree (if any). Thread-safe.
void CancelClaude();
