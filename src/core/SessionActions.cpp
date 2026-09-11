#include "SessionActions.h"
#include "Json.h"
#include "win/StrUtil.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::wstring envW(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring out(n, L'\0');
    out.resize(GetEnvironmentVariableW(name, out.data(), n));
    return out;
}

fs::path homePath() {
    std::wstring p = envW(L"USERPROFILE");
    if (!p.empty()) return fs::path(p);
    wchar_t buf[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, buf)))
        return fs::path(buf);
    return fs::path(L".");
}

bool onPath(const wchar_t* exe) {
    wchar_t found[MAX_PATH]{};
    return SearchPathW(nullptr, exe, nullptr, MAX_PATH, found, nullptr) > 0;
}

std::wstring quotePath(const std::wstring& s) {
    return L"\"" + s + L"\"";
}

std::wstring lastErrorText(DWORD code) {
    LPWSTR buffer = nullptr;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = n && buffer ? std::wstring(buffer, n) : L"";
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) text.pop_back();
    return text;
}

// Starts a command in its own console window.
//
// CreateProcess rather than ShellExecute: wt.exe in WindowsApps is an app
// execution alias, a reparse point the shell cannot open directly, so
// ShellExecuteEx fails it with "access denied".
bool launchConsole(const std::wstring& commandLine, const std::wstring& directory,
                   std::wstring& error) {
    std::wstring mutableCommand = commandLine;  // CreateProcessW may write to it

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE, nullptr,
                        directory.empty() ? nullptr : directory.c_str(), &si, &pi)) {
        error = L"Could not run: " + commandLine + L"\n" + lastErrorText(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Runs a command with no window and waits for it, returning its exit code.
bool runHidden(const std::wstring& commandLine, DWORD& exitCode, std::wstring& error) {
    std::wstring mutableCommand = commandLine;  // CreateProcessW may write to it

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        error = L"Could not run: " + commandLine + L"\n" + lastErrorText(GetLastError());
        return false;
    }

    WaitForSingleObject(pi.hProcess, 60'000);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// Moves a file to the Recycle Bin so a mistaken delete stays recoverable.
bool recycle(const fs::path& path, std::wstring& error) {
    std::wstring from = path.wstring();
    from.push_back(L'\0');  // SHFileOperation wants a double-null terminated list
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    int result = SHFileOperationW(&op);
    if (result != 0 || op.fAnyOperationsAborted) {
        error = L"Could not move to the Recycle Bin: " + path.wstring();
        return false;
    }
    return true;
}

// The transcript is named after the session and lives under one of the project
// directories; which one is not recorded anywhere, so look for it.
fs::path findClaudeTranscript(const std::string& sessionId) {
    fs::path projects = homePath() / L".claude" / L"projects";
    std::error_code ec;
    if (!fs::is_directory(projects, ec)) return {};

    std::wstring name = utf8to16(sessionId) + L".jsonl";
    for (fs::directory_iterator it(projects, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        fs::path candidate = it->path() / name;
        if (fs::exists(candidate, ec)) return candidate;
    }
    return {};
}

// Rewrites history.jsonl without the session's entries, preserving every other
// line byte for byte - including its original line ending.
bool dropFromClaudeHistory(const std::string& sessionId, std::wstring& error) {
    fs::path history = homePath() / L".claude" / L"history.jsonl";
    std::error_code ec;
    if (!fs::exists(history, ec)) return true;  // nothing to do

    std::string text;
    {
        std::ifstream in(history, std::ios::binary);
        if (!in) {
            error = L"Could not read " + history.wstring();
            return false;
        }
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    std::string kept;
    kept.reserve(text.size());
    bool removedAny = false;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        size_t end = (nl == std::string::npos) ? text.size() : nl + 1;
        std::string_view segment(text.data() + pos, end - pos);

        std::string_view body = segment;
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.remove_suffix(1);

        bool drop = false;
        if (!body.empty()) {
            mini::JValue root;
            if (mini::parse(std::string(body), root) && root.isObject())
                drop = root.strOf("sessionId") == sessionId;
        }
        if (drop) removedAny = true;
        else kept.append(segment);

        pos = end;
    }

    if (!removedAny) return true;

    fs::path temp = history;
    temp += L".new";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            error = L"Could not write " + temp.wstring();
            return false;
        }
        out.write(kept.data(), static_cast<std::streamsize>(kept.size()));
        if (!out) {
            error = L"Could not write " + temp.wstring();
            return false;
        }
    }

    // Atomic swap, keeping the previous file as history.jsonl.bak.
    fs::path backup = history;
    backup += L".bak";
    if (!ReplaceFileW(history.c_str(), temp.c_str(), backup.c_str(),
                      REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr)) {
        DWORD code = GetLastError();
        fs::remove(temp, ec);
        error = L"Could not replace " + history.wstring() + L"\n" + lastErrorText(code);
        return false;
    }
    return true;
}

bool deleteClaudeSession(const Session& session, std::wstring& error) {
    fs::path transcript = findClaudeTranscript(session.sessionId);
    if (!transcript.empty() && !recycle(transcript, error)) return false;
    return dropFromClaudeHistory(session.sessionId, error);
}

bool deleteOpenCodeSession(const Session& session, std::wstring& error) {
    if (!onPath(L"opencode.exe")) {
        error = L"opencode was not found on PATH, so the session cannot be deleted.";
        return false;
    }
    std::wstring command =
        L"opencode.exe session delete " + utf8to16(session.sessionId);

    DWORD exitCode = 0;
    if (!runHidden(command, exitCode, error)) return false;
    if (exitCode != 0) {
        error = L"`opencode session delete` failed with exit code " +
                std::to_wstring(exitCode) + L".";
        return false;
    }
    return true;
}

} // namespace

bool SessionActions::resumeInTerminal(const Session& session, std::wstring& error) {
    std::wstring id = utf8to16(session.sessionId);
    std::wstring directory = utf8to16(session.directory);

    std::wstring resume;
    if (session.agent == "Claude") {
        if (!onPath(L"claude.exe")) {
            error = L"claude was not found on PATH.";
            return false;
        }
        resume = L"claude.exe --resume " + id;
    } else if (session.agent == "OpenCode") {
        if (!onPath(L"opencode.exe")) {
            error = L"opencode was not found on PATH.";
            return false;
        }
        resume = L"opencode.exe --session " + id;
    } else {
        error = L"No terminal command is known for agent \"" +
                utf8to16(session.agent) + L"\".";
        return false;
    }

    // A session's directory can be gone; start in the profile rather than fail.
    std::error_code ec;
    if (directory.empty() || !fs::is_directory(fs::path(directory), ec))
        directory = homePath().wstring();

    // cmd /k keeps the window up after the agent exits, so a failure stays
    // readable instead of the window vanishing.
    if (onPath(L"wt.exe")) {
        std::wstring ignored;
        std::wstring viaTerminal =
            L"wt.exe -d " + quotePath(directory) + L" cmd.exe /k " + resume;
        if (launchConsole(viaTerminal, directory, ignored)) return true;
        // Windows Terminal can be present but refuse to start; fall through.
    }
    return launchConsole(L"cmd.exe /k " + resume, directory, error);
}

bool SessionActions::deleteSession(const Session& session, std::wstring& error) {
    if (session.agent == "Claude") return deleteClaudeSession(session, error);
    if (session.agent == "OpenCode") return deleteOpenCodeSession(session, error);
    error = L"Deleting is not supported for agent \"" + utf8to16(session.agent) + L"\".";
    return false;
}
