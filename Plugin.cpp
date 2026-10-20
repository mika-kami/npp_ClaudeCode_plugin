// Claude Code plugin for Notepad++
// Connects Notepad++ to the Claude Code CLI (`claude`) so you can edit the
// current selection / document, ask questions, or let the Claude Code agent
// edit files on disk - all from the Plugins menu.

#include "NppPlugin.h"
#include "ClaudeRunner.h"
#include "resource.h"

#include <atomic>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>

namespace {

const wchar_t kPluginName[] = L"Claude Code";
constexpr UINT WM_CLAUDE_DONE = WM_APP + 1;
constexpr int kFuncCount = 9;

HINSTANCE g_hModule = nullptr;
NppData g_npp{};
HWND g_msgWnd = nullptr;
FuncItem g_funcs[kFuncCount]{};
ShortcutKey g_editKey{true, true, true, 'E'};  // Ctrl+Alt+Shift+E
std::atomic<bool> g_busy{false};
std::thread g_worker;

enum class Kind { Edit, Ask, Agent };

struct Job {
    Kind kind = Kind::Edit;
    std::wstring instruction;
    std::wstring filePath;   // full path, or tab name for unsaved docs
    LRESULT bufferId = 0;
    intptr_t start = 0, end = 0;
    std::string original;    // raw document bytes of [start, end)
    bool docIsUtf8 = true;
    int eolMode = SC_EOL_CRLF;
};

struct Done {
    std::unique_ptr<Job> job;
    RunResult run;
};

struct Config {
    std::wstring claudeCmd = L"claude";
    std::wstring extraArgs;
    size_t maxContextBytes = 256 * 1024;
};

// ---------------------------------------------------------------- strings

std::wstring Widen(const std::string& s, UINT cp = CP_UTF8) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w, UINT cp = CP_UTF8) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(cp, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(cp, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
    return s;
}

std::string Recode(const std::string& s, UINT from, UINT to) {
    return from == to ? s : Narrow(Widen(s, from), to);
}

std::wstring Trim(std::wstring s) {
    const wchar_t* ws = L" \t\r\n";
    size_t b = s.find_first_not_of(ws);
    if (b == std::wstring::npos) return L"";
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

void TrimTrailingNewlines(std::string& s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
}

// Claude sometimes wraps code in ```fences``` despite instructions; remove them.
std::string StripFences(std::string s) {
    size_t b = s.find_first_not_of("\r\n");
    if (b == std::string::npos) return "";
    s.erase(0, b);
    TrimTrailingNewlines(s);
    if (s.compare(0, 3, "```") == 0) {
        size_t nl = s.find('\n');
        size_t last = s.rfind("```");
        if (nl != std::string::npos && last > nl &&
            s.find_first_not_of(" \t\r\n", last + 3) == std::string::npos) {
            s = s.substr(nl + 1, last - (nl + 1));
            TrimTrailingNewlines(s);
        }
    }
    return s;
}

const char* EolString(int mode) {
    return mode == SC_EOL_LF ? "\n" : mode == SC_EOL_CR ? "\r" : "\r\n";
}

std::string NormalizeEol(const std::string& in, int mode) {
    const std::string eol = EolString(mode);
    std::string out;
    out.reserve(in.size() + in.size() / 20);
    for (size_t i = 0; i < in.size(); ++i) {
        char c = in[i];
        if (c == '\r') {
            if (i + 1 < in.size() && in[i + 1] == '\n') ++i;
            out += eol;
        } else if (c == '\n') {
            out += eol;
        } else {
            out += c;
        }
    }
    return out;
}

std::wstring Truncate(const std::wstring& s, size_t max) {
    return s.size() <= max ? s : s.substr(0, max) + L"\n...(truncated)";
}

// ---------------------------------------------------------------- Notepad++ helpers

LRESULT Npp(UINT msg, WPARAM w = 0, LPARAM l = 0) { return SendMessageW(g_npp._nppHandle, msg, w, l); }
LRESULT Sci(HWND h, UINT msg, WPARAM w = 0, LPARAM l = 0) { return SendMessageW(h, msg, w, l); }

HWND CurrentSci() {
    int which = -1;
    Npp(NPPM_GETCURRENTSCINTILLA, 0, reinterpret_cast<LPARAM>(&which));
    return which == 1 ? g_npp._scintillaSecondHandle : g_npp._scintillaMainHandle;
}

std::wstring CurrentPath() {
    std::wstring buf(32768, L'\0');
    Npp(NPPM_GETFULLCURRENTPATH, buf.size(), reinterpret_cast<LPARAM>(&buf[0]));
    buf.resize(wcslen(buf.c_str()));
    return buf;
}

bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring DirOf(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"" : p.substr(0, pos);
}

std::wstring UserProfileDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? buf : L"C:\\";
}

std::wstring WorkingDirFor(const std::wstring& path) {
    return FileExists(path) ? DirOf(path) : UserProfileDir();
}

std::string DocRange(HWND sci, intptr_t a, intptr_t b) {
    auto p = reinterpret_cast<const char*>(Sci(sci, SCI_GETCHARACTERPOINTER));
    return (p && b > a) ? std::string(p + a, static_cast<size_t>(b - a)) : std::string();
}

bool SciIsUtf8(HWND sci) { return Sci(sci, SCI_GETCODEPAGE) == CP_UTF8; }

void SetStatus(const std::wstring& text) {
    Npp(NPPM_SETSTATUSBAR, STATUSBAR_DOC_TYPE, reinterpret_cast<LPARAM>(text.c_str()));
}

void Info(const std::wstring& text) {
    MessageBoxW(g_npp._nppHandle, text.c_str(), kPluginName, MB_OK | MB_ICONINFORMATION);
}
void Error(const std::wstring& text) {
    MessageBoxW(g_npp._nppHandle, text.c_str(), kPluginName, MB_OK | MB_ICONERROR);
}

void OpenInNewTab(const std::string& utf8) {
    Npp(NPPM_MENUCOMMAND, 0, IDM_FILE_NEW);
    HWND sci = CurrentSci();
    std::string text = NormalizeEol(utf8, static_cast<int>(Sci(sci, SCI_GETEOLMODE)));
    if (!SciIsUtf8(sci)) text = Recode(text, CP_UTF8, CP_ACP);
    Sci(sci, SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(text.c_str()));
}

bool CheckSingleSelection(HWND sci) {
    if (Sci(sci, SCI_GETSELECTIONS) > 1 || Sci(sci, SCI_SELECTIONISRECTANGLE)) {
        Error(L"Multiple or rectangular selections are not supported. Please make a single selection.");
        return false;
    }
    return true;
}

bool CheckIdle() {
    if (g_busy) {
        Info(L"Claude is still working on the previous request.\n"
             L"Wait for it to finish or use Plugins > Claude Code > Cancel running request.");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- configuration

std::wstring IniPath() {
    wchar_t dir[MAX_PATH * 2]{};
    Npp(NPPM_GETPLUGINSCONFIGDIR, MAX_PATH * 2, reinterpret_cast<LPARAM>(dir));
    return std::wstring(dir) + L"\\ClaudeCode.ini";
}

void EnsureIni() {
    std::wstring p = IniPath();
    if (FileExists(p)) return;
    CreateDirectoryW(DirOf(p).c_str(), nullptr);
    WritePrivateProfileStringW(L"Claude", L"ClaudeCommand", L"claude", p.c_str());
    WritePrivateProfileStringW(L"Claude", L"ExtraArgs", L"", p.c_str());
    WritePrivateProfileStringW(L"Claude", L"MaxContextKB", L"256", p.c_str());
}

Config LoadConfig() {
    EnsureIni();
    Config c;
    std::wstring p = IniPath();
    wchar_t buf[4096];
    GetPrivateProfileStringW(L"Claude", L"ClaudeCommand", L"claude", buf, 4096, p.c_str());
    c.claudeCmd = Trim(buf);
    if (c.claudeCmd.empty()) c.claudeCmd = L"claude";
    if (c.claudeCmd.find(L' ') != std::wstring::npos && c.claudeCmd[0] != L'"')
        c.claudeCmd = L"\"" + c.claudeCmd + L"\"";
    GetPrivateProfileStringW(L"Claude", L"ExtraArgs", L"", buf, 4096, p.c_str());
    c.extraArgs = Trim(buf);
    c.maxContextBytes = static_cast<size_t>(GetPrivateProfileIntW(L"Claude", L"MaxContextKB", 256, p.c_str())) * 1024;
    return c;
}

std::wstring ClaudeCommandLine(const Config& cfg, const std::wstring& args) {
    std::wstring cmd = cfg.claudeCmd;
    if (!args.empty()) cmd += L" " + args;
    if (!cfg.extraArgs.empty()) cmd += L" " + cfg.extraArgs;
    return cmd;
}

// ---------------------------------------------------------------- instruction dialog

struct PromptData {
    std::wstring title, label, text;
};

INT_PTR CALLBACK PromptProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_INITDIALOG: {
        auto* d = reinterpret_cast<PromptData*>(lp);
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetWindowTextW(dlg, d->title.c_str());
        SetDlgItemTextW(dlg, IDC_LABEL, d->label.c_str());
        HWND edit = GetDlgItem(dlg, IDC_INSTRUCTION);
        SetWindowTextW(edit, d->text.c_str());
        SendMessageW(edit, EM_SETSEL, 0, -1);
        SetFocus(edit);
        return FALSE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            auto* d = reinterpret_cast<PromptData*>(GetWindowLongPtrW(dlg, DWLP_USER));
            HWND edit = GetDlgItem(dlg, IDC_INSTRUCTION);
            int n = GetWindowTextLengthW(edit);
            std::wstring t(n + 1, L'\0');
            GetWindowTextW(edit, &t[0], n + 1);
            t.resize(n);
            t = Trim(t);
            if (t.empty()) {
                MessageBeep(MB_ICONWARNING);
                return TRUE;
            }
            d->text = t;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool AskInstruction(const std::wstring& title, const std::wstring& label, std::wstring& inOut) {
    PromptData d{title, label, inOut};
    if (DialogBoxParamW(g_hModule, MAKEINTRESOURCEW(IDD_PROMPT), g_npp._nppHandle, PromptProc,
                        reinterpret_cast<LPARAM>(&d)) != IDOK)
        return false;
    inOut = d.text;
    return true;
}

// ---------------------------------------------------------------- running jobs

void StartJob(std::unique_ptr<Job> job, const std::string& prompt, const std::wstring& args) {
    if (g_busy.exchange(true)) return;
    if (g_worker.joinable()) g_worker.join();

    const Config cfg = LoadConfig();
    const std::wstring cmd = ClaudeCommandLine(cfg, args);
    const std::wstring cwd = WorkingDirFor(job->filePath);
    SetStatus(L"Claude is working...  (Plugins > Claude Code > Cancel running request)");

    Job* raw = job.release();
    g_worker = std::thread([raw, cmd, cwd, prompt] {
        auto* d = new Done;
        d->job.reset(raw);
        d->run = RunClaude(cmd, cwd, prompt);
        if (!g_msgWnd || !PostMessageW(g_msgWnd, WM_CLAUDE_DONE, 0, reinterpret_cast<LPARAM>(d))) delete d;
    });
}

std::string DocToUtf8(const std::string& raw, bool isUtf8) { return isUtf8 ? raw : Recode(raw, CP_ACP, CP_UTF8); }

std::string FileLabel(const Job& j) {
    return FileExists(j.filePath) ? Narrow(j.filePath) : Narrow(j.filePath) + " (unsaved document)";
}

void ApplyEdit(const Job& j, const RunResult& r) {
    std::string utf8 = StripFences(r.out);
    bool origEndsWithEol = !j.original.empty() && (j.original.back() == '\n' || j.original.back() == '\r');
    TrimTrailingNewlines(utf8);
    utf8 = NormalizeEol(utf8, j.eolMode);
    if (origEndsWithEol) utf8 += EolString(j.eolMode);
    const std::string text = j.docIsUtf8 ? utf8 : Recode(utf8, CP_UTF8, CP_ACP);

    HWND sci = CurrentSci();
    bool unchanged = Npp(NPPM_GETCURRENTBUFFERID) == j.bufferId &&
                     j.end <= Sci(sci, SCI_GETLENGTH) && DocRange(sci, j.start, j.end) == j.original;
    if (!unchanged) {
        OpenInNewTab(utf8);
        Info(L"The document was changed or switched while Claude was working, so the result "
             L"was opened in a new tab instead of being applied.");
        return;
    }
    Sci(sci, SCI_BEGINUNDOACTION);
    Sci(sci, SCI_SETTARGETSTART, j.start);
    Sci(sci, SCI_SETTARGETEND, j.end);
    Sci(sci, SCI_REPLACETARGET, text.size(), reinterpret_cast<LPARAM>(text.data()));
    Sci(sci, SCI_ENDUNDOACTION);
    Sci(sci, SCI_SETSEL, j.start, j.start + static_cast<intptr_t>(text.size()));
    SetStatus(L"Claude: edit applied (Ctrl+Z to undo)");
}

void OnDone(Done& d) {
    if (g_worker.joinable()) g_worker.join();
    g_busy = false;
    const Job& j = *d.job;
    const RunResult& r = d.run;

    if (r.cancelled) {
        SetStatus(L"Claude: request cancelled");
        return;
    }
    if (!r.startError.empty()) {
        SetStatus(L"Claude: failed to start");
        Error(L"Could not start Claude Code:\n" + r.startError);
        return;
    }
    std::string trimmedOut = r.out;
    TrimTrailingNewlines(trimmedOut);
    if (r.exitCode != 0 || (trimmedOut.empty() && j.kind != Kind::Agent)) {
        SetStatus(L"Claude: error");
        std::wstring details = Widen(r.err.empty() ? r.out : r.err);
        Error(L"Claude Code exited with code " + std::to_wstring(r.exitCode) + L".\n\n" +
              Truncate(Trim(details), 2000) +
              L"\n\nCheck that `claude` works in a terminal and that you are logged in. "
              L"You can set the full path in Plugins > Claude Code > Settings.");
        return;
    }

    switch (j.kind) {
    case Kind::Edit:
        ApplyEdit(j, r);
        break;
    case Kind::Ask:
        OpenInNewTab("Question: " + Narrow(j.instruction) + "\nFile: " + FileLabel(j) + "\n\n" + trimmedOut + "\n");
        SetStatus(L"Claude: answer opened in a new tab");
        break;
    case Kind::Agent: {
        // Reload so the editor shows Claude's on-disk edits. Ask first if the user typed meanwhile.
        bool safe = CurrentPath() == j.filePath && !Sci(CurrentSci(), SCI_GETMODIFY);
        Npp(NPPM_RELOADFILE, safe ? FALSE : TRUE, reinterpret_cast<LPARAM>(j.filePath.c_str()));
        SetStatus(L"Claude: agent finished");
        std::wstring summary = Trim(Widen(trimmedOut));
        if (summary.size() > 1500)
            OpenInNewTab("Claude Code agent summary\nTask: " + Narrow(j.instruction) + "\n\n" + trimmedOut + "\n");
        else
            Info(L"Claude Code finished.\n\n" + (summary.empty() ? L"(no summary)" : summary));
        break;
    }
    }
}

LRESULT CALLBACK MsgWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CLAUDE_DONE) {
        std::unique_ptr<Done> d(reinterpret_cast<Done*>(lp));
        OnDone(*d);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------- menu commands

void cmdEditSelection() {
    if (!CheckIdle()) return;
    HWND sci = CurrentSci();
    if (Sci(sci, SCI_GETREADONLY)) { Error(L"The document is read-only."); return; }
    if (!CheckSingleSelection(sci)) return;

    auto job = std::make_unique<Job>();
    job->kind = Kind::Edit;
    job->bufferId = Npp(NPPM_GETCURRENTBUFFERID);
    job->filePath = CurrentPath();
    job->docIsUtf8 = SciIsUtf8(sci);
    job->eolMode = static_cast<int>(Sci(sci, SCI_GETEOLMODE));
    intptr_t s = Sci(sci, SCI_GETSELECTIONSTART), e = Sci(sci, SCI_GETSELECTIONEND);
    const intptr_t len = Sci(sci, SCI_GETLENGTH);
    const bool whole = (s == e);
    if (whole) { s = 0; e = len; }
    job->start = s;
    job->end = e;
    job->original = DocRange(sci, s, e);

    static std::wstring last;
    if (!AskInstruction(L"Edit with Claude",
                        whole ? L"Nothing selected - instruction for the WHOLE document:"
                              : L"Instruction for the selected text:",
                        last))
        return;
    job->instruction = last;

    const Config cfg = LoadConfig();
    std::string p;
    p += "You are being called from a Notepad++ plugin to rewrite text in the user's editor.\n";
    p += "File: " + FileLabel(*job) + "\n";
    p += "Instruction: " + Narrow(job->instruction) + "\n\n";
    p += "Rules:\n"
         "- Reply with ONLY the new text that will replace the target text. Nothing else.\n"
         "- No explanations, no preamble, no markdown code fences.\n"
         "- Keep the existing indentation style and conventions unless the instruction says otherwise.\n"
         "- Do not use tools; everything you need is below.\n\n";
    if (!whole && static_cast<size_t>(len) <= cfg.maxContextBytes) {
        p += "Full file, for context only (do NOT return it):\n<file>\n" +
             DocToUtf8(DocRange(sci, 0, len), job->docIsUtf8) + "\n</file>\n\n";
    }
    if (job->original.empty())
        p += "The document is currently empty. Write its new content.\n";
    else
        p += std::string(whole ? "Target text (the whole document):" : "Target text (the selection):") +
             "\n<target>\n" + DocToUtf8(job->original, job->docIsUtf8) + "\n</target>\n";

    StartJob(std::move(job), p, L"-p --output-format text");
}

void cmdAsk() {
    if (!CheckIdle()) return;
    HWND sci = CurrentSci();
    if (!CheckSingleSelection(sci)) return;

    auto job = std::make_unique<Job>();
    job->kind = Kind::Ask;
    job->filePath = CurrentPath();
    const bool utf8 = SciIsUtf8(sci);
    intptr_t s = Sci(sci, SCI_GETSELECTIONSTART), e = Sci(sci, SCI_GETSELECTIONEND);
    const intptr_t len = Sci(sci, SCI_GETLENGTH);
    const bool hasSel = s != e;

    static std::wstring last;
    if (!AskInstruction(L"Ask Claude", hasSel ? L"Question about the selected text:" : L"Question about this file:",
                        last))
        return;
    job->instruction = last;

    const Config cfg = LoadConfig();
    std::string p = "The user is working in Notepad++ on: " + FileLabel(*job) + "\n";
    p += "Question: " + Narrow(job->instruction) + "\n\n";
    if (static_cast<size_t>(len) <= cfg.maxContextBytes)
        p += "File content:\n<file>\n" + DocToUtf8(DocRange(sci, 0, len), utf8) + "\n</file>\n\n";
    else
        p += "(The file is large and was not included; read it from disk if needed.)\n\n";
    if (hasSel) p += "The question refers to this selection:\n<selection>\n" + DocToUtf8(DocRange(sci, s, e), utf8) + "\n</selection>\n";
    p += "Answer concisely. Markdown is fine. Do not modify any files.\n";

    StartJob(std::move(job), p, L"-p --output-format text");
}

void cmdAgent() {
    if (!CheckIdle()) return;
    std::wstring path = CurrentPath();
    if (!FileExists(path)) { Error(L"Save the document to disk first - the agent edits files on disk."); return; }
    HWND sci = CurrentSci();
    if (Sci(sci, SCI_GETMODIFY)) Npp(NPPM_SAVECURRENTFILE);

    auto job = std::make_unique<Job>();
    job->kind = Kind::Agent;
    job->filePath = path;
    intptr_t s = Sci(sci, SCI_GETSELECTIONSTART), e = Sci(sci, SCI_GETSELECTIONEND);

    static std::wstring last;
    if (!AskInstruction(L"Claude Code agent",
                        L"Task for Claude Code (it may edit this file and others in its folder):", last))
        return;
    job->instruction = last;

    std::string p = "You were started from the Notepad++ plugin. The user has this file open: " + Narrow(path) + "\n";
    if (s != e) {
        intptr_t l1 = Sci(sci, SCI_LINEFROMPOSITION, s) + 1, l2 = Sci(sci, SCI_LINEFROMPOSITION, e) + 1;
        p += "They have lines " + std::to_string(l1) + "-" + std::to_string(l2) + " selected.\n";
    }
    p += "Task: " + Narrow(job->instruction) + "\n\n";
    p += "Make the changes directly in the file(s) with your edit tools. "
         "When finished, reply with a short plain-text summary of what you changed.\n";

    StartJob(std::move(job), p, L"-p --output-format text --permission-mode acceptEdits");
}

void cmdOpenTerminal() {
    const Config cfg = LoadConfig();
    const std::wstring dir = WorkingDirFor(CurrentPath());
    const std::wstring claude = ClaudeCommandLine(cfg, L"");

    std::wstring cmdline;
    wchar_t wt[MAX_PATH];
    if (SearchPathW(nullptr, L"wt.exe", nullptr, MAX_PATH, wt, nullptr)) {
        std::wstring d = dir;
        if (!d.empty() && d.back() == L'\\') d += L'.';  // avoid "C:\" escaping the quote
        cmdline = L"\"" + std::wstring(wt) + L"\" -d \"" + d + L"\" cmd.exe /k " + claude;
    } else {
        wchar_t comspec[MAX_PATH];
        if (!GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH)) wcscpy(comspec, L"cmd.exe");
        cmdline = L"\"" + std::wstring(comspec) + L"\" /k " + claude;
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, &cmdline[0], nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, dir.c_str(),
                        &si, &pi)) {
        Error(L"Could not open a terminal.\nCommand: " + cmdline);
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
}

void cmdCancel() {
    if (!g_busy) { Info(L"No Claude request is running."); return; }
    CancelClaude();
}

void cmdSettings() {
    EnsureIni();
    std::wstring p = IniPath();
    Npp(NPPM_DOOPEN, 0, reinterpret_cast<LPARAM>(p.c_str()));
}

void cmdAbout() {
    Info(L"Claude Code for Notepad++ 1.0\n\n"
         L"Edit selection with Claude: rewrites the selection (or the whole document) in place - undo with Ctrl+Z.\n"
         L"Ask Claude: answers a question about the file in a new tab.\n"
         L"Agent: runs Claude Code on disk with auto-accepted edits, then reloads the file.\n"
         L"Open terminal: starts an interactive Claude Code session in the file's folder.\n\n"
         L"Requires the Claude Code CLI (`claude`) installed and logged in.");
}

void SetItem(int i, const wchar_t* name, PFUNCPLUGINCMD fn, ShortcutKey* key = nullptr) {
    lstrcpynW(g_funcs[i]._itemName, name, menuItemSize);
    g_funcs[i]._pFunc = fn;
    g_funcs[i]._init2Check = false;
    g_funcs[i]._pShKey = key;
}

void InitMenu() {
    SetItem(0, L"Edit selection with Claude...", cmdEditSelection, &g_editKey);
    SetItem(1, L"Ask Claude about this file...", cmdAsk);
    SetItem(2, L"Claude Code agent: edit files...", cmdAgent);
    SetItem(3, L"", nullptr);  // separator
    SetItem(4, L"Open Claude Code terminal here", cmdOpenTerminal);
    SetItem(5, L"Cancel running request", cmdCancel);
    SetItem(6, L"", nullptr);  // separator
    SetItem(7, L"Settings (ClaudeCode.ini)", cmdSettings);
    SetItem(8, L"About", cmdAbout);
}

}  // namespace

// ---------------------------------------------------------------- exported plugin interface

BOOL APIENTRY DllMain(HINSTANCE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hModule = hModule;
        DisableThreadLibraryCalls(hModule);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) void __cdecl setInfo(NppData data) {
    g_npp = data;
    InitMenu();
    WNDCLASSW wc{};
    wc.lpfnWndProc = MsgWndProc;
    wc.hInstance = g_hModule;
    wc.lpszClassName = L"NppClaudeCodeMsgWnd";
    RegisterClassW(&wc);
    g_msgWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hModule, nullptr);
}

extern "C" __declspec(dllexport) const wchar_t* __cdecl getName() { return kPluginName; }

extern "C" __declspec(dllexport) FuncItem* __cdecl getFuncsArray(int* nbF) {
    *nbF = kFuncCount;
    return g_funcs;
}

extern "C" __declspec(dllexport) void __cdecl beNotified(SCNotification* n) {
    if (n->nmhdr.hwndFrom != g_npp._nppHandle) return;
    if (n->nmhdr.code == NPPN_SHUTDOWN) {
        CancelClaude();
        if (g_worker.joinable()) g_worker.join();
        if (g_msgWnd) {
            HWND w = g_msgWnd;
            g_msgWnd = nullptr;
            DestroyWindow(w);
        }
    }
}

extern "C" __declspec(dllexport) LRESULT __cdecl messageProc(UINT, WPARAM, LPARAM) { return TRUE; }

extern "C" __declspec(dllexport) BOOL __cdecl isUnicode() { return TRUE; }
