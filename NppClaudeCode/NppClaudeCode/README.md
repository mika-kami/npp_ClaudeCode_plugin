# Claude Code for Notepad++

A Notepad++ plugin that connects the editor to the **Claude Code CLI** (`claude`).
Everything is in **Plugins → Claude Code**:

| Command | What it does |
|---|---|
| **Edit selection with Claude...** (`Ctrl+Alt+Shift+E`) | Type an instruction; Claude rewrites the selection (or the whole document if nothing is selected) **in place**. One `Ctrl+Z` undoes it. |
| **Ask Claude about this file...** | Ask a question about the file/selection; the answer opens in a new tab. |
| **Claude Code agent: edit files...** | Saves the file, runs Claude Code as an agent in the file's folder with edits auto-accepted, then reloads the file and shows a summary. Good for multi-file changes, refactors, adding tests. |
| **Open Claude Code terminal here** | Opens an interactive `claude` session (Windows Terminal if installed, otherwise cmd) in the current file's folder. Edits it makes on disk show up in Notepad++. |
| **Cancel running request** | Kills the running Claude process (and its children). |
| **Settings (ClaudeCode.ini)** | Opens the config file in Notepad++. |

Requests run in the background, so Notepad++ stays responsive; progress is shown in the status bar.
If you edit or switch documents while an in-place edit is running, the result is opened in a new tab instead of overwriting your text.

## Requirements

- Windows, Notepad++ 8.x **64-bit** (the DLL must match Notepad++'s architecture).
- Claude Code installed and logged in. Test in a terminal first: `claude -p "say hi"` must print an answer.

## Install

**Option A – prebuilt DLL:** copy `prebuilt/x64/ClaudeCode.dll` to

```
C:\Program Files\Notepad++\plugins\ClaudeCode\ClaudeCode.dll
```

The folder name **must** be `ClaudeCode` (same as the DLL). Restart Notepad++.

**Option B – build it yourself** (Visual Studio 2022 with "Desktop development with C++", or Build Tools + CMake):

```bat
cmake -B build -A x64
cmake --build build --config Release
```

Then copy `build\Release\ClaudeCode.dll` as above. For 32-bit Notepad++ use `-A Win32`.
MinGW-w64 also works: `cmake -B build -G "MinGW Makefiles" && cmake --build build`.

## Recommended Notepad++ setting

So files changed by Claude Code (agent mode or the terminal) refresh automatically:
**Settings → Preferences → MISC. → File Status Auto-Detection** → *Enable for all opened files* and tick *Update silently*.

## Configuration

`%AppData%\Notepad++\plugins\config\ClaudeCode.ini` (created on first use):

```ini
[Claude]
; Command used to start Claude Code. Use a full path if Notepad++ can't find it, e.g.
; ClaudeCommand=C:\Users\me\.local\bin\claude.exe
ClaudeCommand=claude
; Extra CLI arguments appended to every call, e.g. --model sonnet
ExtraArgs=
; Files up to this size are sent as context with in-place edits / questions
MaxContextKB=256
```

## How it works

The plugin runs `claude -p --output-format text` through `cmd.exe` and sends the prompt (instruction + selected text + file context) on **stdin**, so there are no command-line quoting problems and claude.cmd (npm) or claude.exe (native installer) both work. The working directory is the file's folder, so your project's `CLAUDE.md` and settings apply.
Agent mode adds `--permission-mode acceptEdits`: Claude can edit files without asking, while shell commands that need approval are denied in non-interactive mode.
Each run lives in a Windows Job Object, so Cancel and closing Notepad++ kill the whole process tree.

## Troubleshooting

- **"exited with code 1 / 'claude' is not recognized"** – Notepad++ was started before Claude Code was added to PATH. Restart Notepad++ (or log off/on) or set `ClaudeCommand` to the full path.
- **Not logged in / auth errors** – run `claude` once in a terminal and log in.
- **Git Bash errors on Windows** – Claude Code may need Git for Windows; follow the error message from `claude` in a terminal.
- **Plugin not shown** – check the folder name, and that you used the x64 DLL with 64-bit Notepad++.

## Safety notes

- In-place edits are a single undo step, but **agent-mode changes are made on disk** – use git or backups for anything important.
- Text you send (selection and up to `MaxContextKB` of the file) goes to Claude through your Claude Code account.
