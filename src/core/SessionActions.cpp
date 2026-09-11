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

// Claude Code injects these into everything it spawns. Inherited by a session
// resumed from here, they make it believe it is a nested child of the session
// that launched this app, which turns transcript saving off - so the resumed
// session would never be recorded and would vanish from this very list.
const wchar_t* const kParentSessionMarkers[] = {
    L"CLAUDECODE",
    L"CLAUDE_PID",
    L"CLAUDE_CODE_CHILD_SESSION",
    L"CLAUDE_CODE_SESSION_ID",
    L"CLAUDE_CODE_BRIDGE_SESSION_ID",
    L"CLAUDE_CODE_MESSAGING_SOCKET",
    L"CLAUDE_CODE_MESSAGING_TOKEN",
    L"CLAUDE_CODE_ENTRYPOINT",
};

// The current environment minus those markers, as the double-null-terminated
// block CREATE_UNICODE_ENVIRONMENT expects. Empty when there is nothing to
// strip, so the caller can just inherit.
std::wstring environmentWithoutParentSession() {
    LPWCH environment = GetEnvironmentStringsW();
    if (!environment) return {};

    std::wstring block;
    bool stripped = false;
    for (LPWCH p = environment; *p;) {
        std::wstring entry(p);
        p += entry.size() + 1;

        // Entries may start with '=' (the per-drive current directories), so
        // the name ends at the *next* '='.
        size_t eq = entry.find(L'=', 1);
        std::wstring name = (eq == std::wstring::npos) ? entry : entry.substr(0, eq);

        bool drop = false;
        for (const wchar_t* marker : kParentSessionMarkers) {
            if (_wcsicmp(name.c_str(), marker) == 0) { drop = true; break; }
        }
        if (drop) {
            stripped = true;
        } else {
            block += entry;
            block.push_back(L'\0');
        }
    }
    FreeEnvironmentStringsW(environment);

    if (!stripped) return {};
    block.push_back(L'\0');
    return block;
}

// Windows Terminal's settings file is JSONC - it may carry // and /* */
// comments, which a strict JSON parser would choke on.
std::string stripJsonComments(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    bool inString = false, escaped = false;

    for (size_t i = 0; i < text.size(); i++) {
        char c = text[i];
        if (inString) {
            out.push_back(c);
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
            out.push_back(c);
            continue;
        }
        if (c == '/' && i + 1 < text.size()) {
            if (text[i + 1] == '/') {
                while (i < text.size() && text[i] != '\n') i++;
                if (i < text.size()) out.push_back('\n');
                continue;
            }
            if (text[i + 1] == '*') {
                i += 2;
                while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) i++;
                i++;  // the loop's i++ steps past '/'
                continue;
            }
        }
        out.push_back(c);
    }
    return out;
}

// Windows Terminal applies a profile's appearance even when it is told to run a
// different command, but only when the profile is named: given a bare command
// line it falls back to a plain console look. So the user's default profile is
// read from the settings file and passed through explicitly - otherwise a
// session opens in the wrong colours.
std::wstring defaultTerminalProfile() {
    const fs::path candidates[] = {
        fs::path(envW(L"LOCALAPPDATA")) / L"Packages" /
            L"Microsoft.WindowsTerminal_8wekyb3d8bbwe" / L"LocalState" / L"settings.json",
        fs::path(envW(L"LOCALAPPDATA")) / L"Packages" /
            L"Microsoft.WindowsTerminalPreview_8wekyb3d8bbwe" / L"LocalState" / L"settings.json",
        fs::path(envW(L"LOCALAPPDATA")) / L"Microsoft" / L"Windows Terminal" / L"settings.json",
    };

    std::error_code ec;
    for (const fs::path& path : candidates) {
        if (!fs::exists(path, ec)) continue;
        std::ifstream in(path, std::ios::binary);
        if (!in) continue;
        std::string text((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());

        mini::JValue root;
        if (!mini::parse(stripJsonComments(text), root) || !root.isObject()) continue;
        std::string guid = root.strOf("defaultProfile");
        if (!guid.empty()) return utf8to16(guid);
    }
    return {};
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

    std::wstring environment = environmentWithoutParentSession();
    if (!CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE | CREATE_UNICODE_ENVIRONMENT,
                        environment.empty() ? nullptr : environment.data(),
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

    if (!onPath(L"wt.exe")) {
        error = L"Windows Terminal (wt.exe) was not found.";
        return false;
    }

    // The agent runs as the tab's own process - no shell wrapper - under the
    // user's default profile, so a resumed session looks exactly like one
    // started by hand.
    std::wstring command = L"wt.exe";
    std::wstring profile = defaultTerminalProfile();
    if (!profile.empty()) command += L" -p " + quotePath(profile);
    command += L" -d " + quotePath(directory) + L" " + resume;

    return launchConsole(command, directory, error);
}

bool SessionActions::deleteSession(const Session& session, std::wstring& error) {
    if (session.agent == "Claude") return deleteClaudeSession(session, error);
    if (session.agent == "OpenCode") return deleteOpenCodeSession(session, error);
    error = L"Deleting is not supported for agent \"" + utf8to16(session.agent) + L"\".";
    return false;
}
