#include "SessionActions.h"
#include "Json.h"
#include "win/StrUtil.h"

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <sqlite3.h>

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

// %LOCALAPPDATA%\AISessions is where Settings also keeps settings.json.
fs::path deletionLogPath() {
    std::wstring base = envW(L"LOCALAPPDATA");
    fs::path dir = base.empty() ? fs::path(L".") / L"AISessions"
                                : fs::path(base) / L"AISessions";
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / L"deletions.log";
}

// Escapes a string for use inside a JSON double-quoted string - just what a
// session's title or directory can actually contain (quotes, backslashes,
// control characters); not a general-purpose JSON writer.
std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    return out;
}

std::wstring quotePath(const std::wstring& s) {
    // CommandLineToArgvW treats a backslash specially only right before a
    // closing quote: an odd run escapes the quote itself instead of ending
    // the argument, silently merging it with everything that follows on the
    // command line. A drive root ("D:\") is the one path this app quotes
    // that can end in a backslash, so double it to keep the quote closing.
    std::wstring escaped = s;
    if (!escaped.empty() && escaped.back() == L'\\') escaped += L'\\';
    return L"\"" + escaped + L"\"";
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

// Rewrites history.jsonl without the given sessions' entries, preserving every
// other line byte for byte - including its original line ending.
bool dropFromClaudeHistory(const std::unordered_set<std::string>& sessionIds,
                           std::wstring& error) {
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
                drop = sessionIds.count(root.strOf("sessionId")) > 0;
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

// %LOCALAPPDATA%\share\opencode\opencode.db is where the Windows build keeps
// its database; the ~/.local/share layout is the fallback. Mirrors
// SessionLoader's own lookup, which has no header to share this from.
fs::path openCodeDbPath() {
    fs::path primary = fs::path(envW(L"LOCALAPPDATA")) / L"share" / L"opencode" / L"opencode.db";
    fs::path fallback = homePath() / L".local" / L"share" / L"opencode" / L"opencode.db";
    std::error_code ec;
    if (fs::exists(primary, ec)) return primary;
    if (fs::exists(fallback, ec)) return fallback;
    return {};
}

// Copies one sqlite database into another via the online backup API rather
// than a raw file copy: it produces a consistent snapshot regardless of a
// concurrent writer or WAL activity on either end, and - used in reverse for
// restoring - writes through sqlite's normal locking instead of clobbering a
// database something else might have open.
bool sqliteBackup(const fs::path& srcPath, int srcFlags, const fs::path& dstPath,
                  int dstFlags, std::wstring& error) {
    sqlite3* src = nullptr;
    sqlite3* dst = nullptr;
    std::string srcUtf8 = utf16to8(srcPath.wstring());
    std::string dstUtf8 = utf16to8(dstPath.wstring());

    bool ok = sqlite3_open_v2(srcUtf8.c_str(), &src, srcFlags, nullptr) == SQLITE_OK;
    if (ok) ok = sqlite3_open_v2(dstUtf8.c_str(), &dst, dstFlags, nullptr) == SQLITE_OK;

    sqlite3_backup* backup = ok ? sqlite3_backup_init(dst, "main", src, "main") : nullptr;
    if (ok && !backup) ok = false;

    if (backup) {
        int rc;
        int busyRetries = 0;
        do {
            rc = sqlite3_backup_step(backup, -1);
            if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
                if (++busyRetries > 50) break;  // ~5s waiting for the other side to let go
                Sleep(100);
            }
        } while (rc == SQLITE_OK || rc == SQLITE_BUSY || rc == SQLITE_LOCKED);
        ok = (rc == SQLITE_DONE);
        sqlite3_backup_finish(backup);
    }
    if (!ok) {
        const char* detail = dst ? sqlite3_errmsg(dst) : "sqlite3_open failed";
        error = L"Could not copy " + srcPath.wstring() + L" to " + dstPath.wstring() +
                L": " + utf8to16(detail);
    }
    sqlite3_close(dst);
    sqlite3_close(src);
    return ok;
}

// Builds the agent's own invocation: `claude.exe --resume <id>` or a bare
// `claude.exe` when sessionId is empty (a new session); same shape for
// opencode. Shared by resume and new-session launches.
bool buildAgentCommand(const std::string& agent, const std::wstring& sessionId,
                       std::wstring& command, std::wstring& error) {
    if (agent == "Claude") {
        if (!onPath(L"claude.exe")) {
            error = L"claude was not found on PATH.";
            return false;
        }
        command = L"claude.exe";
        if (!sessionId.empty()) command += L" --resume " + sessionId;
    } else if (agent == "OpenCode") {
        if (!onPath(L"opencode.exe")) {
            error = L"opencode was not found on PATH.";
            return false;
        }
        command = L"opencode.exe";
        if (!sessionId.empty()) command += L" --session " + sessionId;
    } else {
        error = L"No terminal command is known for agent \"" + utf8to16(agent) + L"\".";
        return false;
    }
    return true;
}

// Opens a Windows Terminal tab in `directory` running `agentCommand`, under
// the user's default profile with no shell wrapper - shared by resume and
// new-session launches so both look exactly like something started by hand.
bool launchInTerminal(const std::wstring& agentCommand, std::wstring directory,
                      std::wstring& error) {
    // A session's directory can be gone; start in the profile rather than fail.
    std::error_code ec;
    if (directory.empty() || !fs::is_directory(fs::path(directory), ec))
        directory = homePath().wstring();

    if (!onPath(L"wt.exe")) {
        error = L"Windows Terminal (wt.exe) was not found.";
        return false;
    }

    std::wstring command = L"wt.exe";
    std::wstring profile = defaultTerminalProfile();
    if (!profile.empty()) command += L" -p " + quotePath(profile);
    command += L" -d " + quotePath(directory) + L" " + agentCommand;

    return launchConsole(command, directory, error);
}

} // namespace

bool SessionActions::resumeInTerminal(const Session& session, std::wstring& error) {
    std::wstring command;
    if (!buildAgentCommand(session.agent, utf8to16(session.sessionId), command, error))
        return false;
    return launchInTerminal(command, utf8to16(session.directory), error);
}

bool SessionActions::startNewSession(const std::string& agent, const std::wstring& directory,
                                     std::wstring& error) {
    std::wstring command;
    if (!buildAgentCommand(agent, std::wstring(), command, error)) return false;
    return launchInTerminal(command, directory, error);
}

void SessionActions::logDeletion(const Session& session, const std::string& outcome) {
    std::ofstream out(deletionLogPath(), std::ios::binary | std::ios::app);
    if (!out) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char stamp[32];
    std::snprintf(stamp, sizeof(stamp), "%04d-%02d-%02d %02d:%02d:%02d", st.wYear,
                 st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    out << "{\"time\":\"" << stamp << "\",\"agent\":\"" << jsonEscape(session.agent)
        << "\",\"sessionId\":\"" << jsonEscape(session.sessionId) << "\",\"title\":\""
        << jsonEscape(session.title) << "\",\"directory\":\"" << jsonEscape(session.directory)
        << "\",\"outcome\":\"" << jsonEscape(outcome) << "\"}\n";
}

bool SessionActions::deleteClaudeSessions(const std::vector<Session>& sessions,
                                          long long& freedBytes, std::wstring& error) {
    freedBytes = 0;
    if (sessions.empty()) return true;

    std::unordered_set<std::string> sessionIds;
    for (const Session& s : sessions) sessionIds.insert(s.sessionId);

    std::error_code ec;
    // Any transcript that does still exist goes to the Recycle Bin first, so a
    // caller passing a mixed list is not silently unlinking data.
    for (const std::string& id : sessionIds) {
        fs::path transcript = findClaudeTranscript(id);
        if (transcript.empty()) continue;
        auto bytes = fs::file_size(transcript, ec);
        if (!recycle(transcript, error)) {
            for (const Session& s : sessions) logDeletion(s, "failed: " + utf16to8(error));
            return false;
        }
        if (!ec) freedBytes += static_cast<long long>(bytes);
    }

    fs::path history = homePath() / L".claude" / L"history.jsonl";
    auto before = fs::file_size(history, ec);
    if (ec) before = 0;
    if (!dropFromClaudeHistory(sessionIds, error)) {
        for (const Session& s : sessions) logDeletion(s, "failed: " + utf16to8(error));
        return false;
    }
    auto after = fs::file_size(history, ec);
    if (!ec && before > after) freedBytes += static_cast<long long>(before - after);

    for (const Session& s : sessions) logDeletion(s, "deleted");
    return true;
}

bool SessionActions::deleteOpenCodeSessions(const std::vector<Session>& sessions,
                                            std::wstring& error) {
    if (sessions.empty()) return true;
    if (!onPath(L"opencode.exe")) {
        error = L"opencode was not found on PATH, so sessions cannot be deleted.";
        return false;
    }

    fs::path dbPath = openCodeDbPath();
    std::error_code ec;
    if (dbPath.empty() || !fs::exists(dbPath, ec)) {
        error = L"Could not find opencode.db, so a safety backup cannot be made "
                L"first; refusing to delete.";
        return false;
    }

    fs::path backupPath = dbPath;
    backupPath += L".bak";
    if (!sqliteBackup(dbPath, SQLITE_OPEN_READONLY, backupPath,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, error)) {
        error = L"Could not back up opencode.db before deleting - refusing to delete "
                L"without one:\n" + error;
        return false;
    }

    std::vector<const Session*> succeeded;
    for (size_t i = 0; i < sessions.size(); i++) {
        const Session& session = sessions[i];
        std::wstring command = L"opencode.exe session delete " + utf8to16(session.sessionId);
        DWORD exitCode = 0;
        std::wstring runError;
        bool ran = runHidden(command, exitCode, runError);
        if (ran && exitCode == 0) {
            succeeded.push_back(&session);
            continue;
        }

        std::wstring failure = ran
            ? (L"`opencode session delete` failed for " + utf8to16(session.sessionId) +
               L" with exit code " + std::to_wstring(exitCode) + L".")
            : (utf8to16(session.sessionId) + L": " + runError);

        std::wstring restoreError;
        bool restored = sqliteBackup(backupPath, SQLITE_OPEN_READONLY, dbPath,
                                     SQLITE_OPEN_READWRITE, restoreError);
        if (restored) {
            error = failure + L"\n\nRestored opencode.db from the backup taken just "
                    L"before this batch, so nothing in it was lost.";
            for (const Session* s : succeeded)
                logDeletion(*s, "deleted, then restored (a later session in the same "
                                "batch failed)");
            logDeletion(session, "failed: " + utf16to8(failure) + " - batch restored");
            for (size_t j = i + 1; j < sessions.size(); j++)
                logDeletion(sessions[j], "skipped (batch aborted before reaching it)");
        } else {
            error = failure + L"\n\nCould not restore the backup either: " + restoreError +
                    L"\nThe backup is still at " + backupPath.wstring();
            for (const Session* s : succeeded) logDeletion(*s, "deleted (batch then failed; "
                                                                "restore ALSO failed)");
            logDeletion(session, "failed: " + utf16to8(failure) +
                                     " - restore ALSO failed: " + utf16to8(restoreError));
            for (size_t j = i + 1; j < sessions.size(); j++)
                logDeletion(sessions[j], "skipped (batch aborted, restore failed - state "
                                          "uncertain, see backup file)");
        }
        return false;
    }

    fs::remove(backupPath, ec);  // whole batch succeeded, no longer needed
    for (const Session& s : sessions) logDeletion(s, "deleted");
    return true;
}

bool SessionActions::deleteSession(const Session& session, std::wstring& error) {
    if (session.agent == "Claude") {
        long long freedBytes = 0;
        return deleteClaudeSessions({session}, freedBytes, error);
    }
    if (session.agent == "OpenCode")
        return deleteOpenCodeSessions({session}, error);
    error = L"Deleting is not supported for agent \"" + utf8to16(session.agent) + L"\".";
    return false;
}
