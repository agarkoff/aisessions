#include "SessionLoader.h"
#include "Json.h"
#include "win/StrUtil.h"

#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <string_view>

#include <sqlite3.h>

namespace fs = std::filesystem;

namespace {

std::wstring envW(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring out(n, L'\0');
    DWORD got = GetEnvironmentVariableW(name, out.data(), n);
    out.resize(got);
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

fs::path localAppData() {
    std::wstring p = envW(L"LOCALAPPDATA");
    if (!p.empty()) return fs::path(p);
    wchar_t buf[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf)))
        return fs::path(buf);
    return fs::path(L".");
}

// Reads a UTF-8 text file line by line, handing each non-empty line to `fn` as
// a view into an internal buffer. The leading BOM is stripped from the first
// line only; CRLF endings are normalised. Returns false when the file could
// not be opened.
//
// Block reads plus memchr rather than std::getline: the transcript files run to
// hundreds of megabytes and getline's per-line allocation dominated start-up.
template <typename Fn>
bool forEachLine(const fs::path& path, Fn fn) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    constexpr size_t kChunk = 1u << 20;
    std::vector<char> buf(kChunk);
    std::string carry;  // partial line spanning a chunk boundary
    bool first = true;

    auto emit = [&](const char* data, size_t size) {
        if (size && data[size - 1] == '\r') --size;
        if (first) {
            first = false;
            if (size >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
                static_cast<unsigned char>(data[1]) == 0xBB &&
                static_cast<unsigned char>(data[2]) == 0xBF) {
                data += 3;
                size -= 3;
            }
        }
        if (size) fn(std::string_view(data, size));
    };

    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(kChunk));
        std::streamsize got = in.gcount();
        if (got <= 0) break;

        const char* p = buf.data();
        const char* end = p + got;
        while (p < end) {
            const char* nl = static_cast<const char*>(memchr(p, '\n', static_cast<size_t>(end - p)));
            if (!nl) {
                carry.append(p, static_cast<size_t>(end - p));
                break;
            }
            if (carry.empty()) {
                emit(p, static_cast<size_t>(nl - p));
            } else {
                carry.append(p, static_cast<size_t>(nl - p));
                emit(carry.data(), carry.size());
                carry.clear();
            }
            p = nl + 1;
        }
    }
    if (!carry.empty()) emit(carry.data(), carry.size());
    return true;
}

// Shortens to at most `maxChars` code points without splitting a multi-byte
// sequence. Cutting at a byte offset left a broken character - rendered as a
// replacement glyph - at the end of every long non-ASCII title, and counted
// Cyrillic titles as twice as long as they look.
std::string truncateUtf8(const std::string& s, size_t maxChars) {
    size_t i = 0;
    for (size_t chars = 0; chars < maxChars && i < s.size(); ++chars) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        i += len;
    }
    if (i >= s.size()) return s;
    return s.substr(0, i) + "\xE2\x80\xA6";  // horizontal ellipsis
}

// Pulls the value of the first "model":"claude-..." on a transcript line. A
// substring scan rather than a JSON parse: assistant lines are the largest in
// the file, and the value is a plain identifier.
std::string_view extractModel(std::string_view line) {
    static constexpr std::string_view key = "\"model\":\"";
    size_t at = line.find(key);
    while (at != std::string_view::npos) {
        size_t start = at + key.size();
        size_t end = line.find('"', start);
        if (end == std::string_view::npos) break;
        std::string_view value = line.substr(start, end - start);
        if (value.rfind("claude-", 0) == 0) return value;
        at = line.find(key, end);
    }
    return {};
}

} // namespace

std::vector<Session> SessionLoader::loadAll() {
    std::vector<Session> sessions = loadClaude();
    std::vector<Session> open = loadOpenCode();
    sessions.insert(sessions.end(), std::make_move_iterator(open.begin()),
                    std::make_move_iterator(open.end()));

    std::sort(sessions.begin(), sessions.end(),
        [](const Session& a, const Session& b) { return a.updatedMs > b.updatedMs; });
    return sessions;
}

std::vector<Session> SessionLoader::loadClaude() {
    fs::path historyFile = homePath() / L".claude" / L"history.jsonl";
    std::vector<Session> result;

    struct Info { std::string dir; long long created = 0; long long updated = 0; std::string prompt; };
    std::unordered_map<std::string, Info> sessions;
    std::unordered_map<std::string, std::string> titles;
    std::unordered_map<std::string, long long> sizes;   // transcript bytes by id
    std::unordered_map<std::string, std::string> models; // last model used, by id

    forEachLine(historyFile, [&](std::string_view line) {
        mini::JValue root;
        if (!mini::parse(std::string(line), root) || !root.isObject()) return;
        std::string sid = root.strOf("sessionId");
        if (sid.empty()) return;

        long long ts = root.intOf("timestamp");
        std::string proj = root.strOf("project");
        std::string prompt = root.strOf("display");

        auto it = sessions.find(sid);
        if (it != sessions.end()) {
            Info& info = it->second;
            if (info.dir.empty()) info.dir = proj;
            if (info.created == 0) info.created = ts;
            if (ts > info.updated) info.updated = ts;
            if (info.prompt.empty()) info.prompt = prompt;
        } else {
            sessions.emplace(sid, Info{ proj, ts, ts, prompt });
        }
    });

    // Scan project transcripts for AI-generated titles. These files hold every
    // message of every session and run to hundreds of megabytes, so reject
    // lines with a substring test before paying for a JSON parse.
    fs::path projectsDir = homePath() / L".claude" / L"projects";
    std::error_code ec;
    if (fs::is_directory(projectsDir, ec)) {
        for (fs::directory_iterator projIt(projectsDir, ec), end; !ec && projIt != end;
             projIt.increment(ec)) {
            if (!projIt->is_directory(ec)) continue;
            std::error_code fileEc;
            for (fs::directory_iterator fileIt(projIt->path(), fileEc); !fileEc && fileIt != end;
                 fileIt.increment(fileEc)) {
                if (fileIt->path().extension() != L".jsonl") continue;
                // The transcript is named after its session; its size is the
                // session's size, and it is free to pick up during this scan.
                std::string transcriptId = utf16to8(fileIt->path().stem().wstring());
                std::error_code sizeEc;
                auto bytes = fileIt->file_size(sizeEc);
                if (!sizeEc) sizes[transcriptId] = static_cast<long long>(bytes);

                forEachLine(fileIt->path(), [&](std::string_view line) {
                    // Cheap rejects first: only a handful of lines in these
                    // files matter, and parsing the rest is pure waste.
                    if (line.find("\"ai-title\"") != std::string_view::npos) {
                        mini::JValue doc;
                        if (!mini::parse(std::string(line), doc) || !doc.isObject()) return;
                        if (doc.strOf("type") != "ai-title") return;
                        std::string s = doc.strOf("sessionId");
                        std::string t = doc.strOf("aiTitle");
                        if (!s.empty() && !t.empty()) titles[s] = std::move(t);
                        return;
                    }

                    // The model is recorded on every assistant message. A
                    // session can switch models, so keep the last one seen -
                    // that is what it is running on now, which matches what the
                    // OpenCode column shows. The "claude-" check skips the
                    // "<synthetic>" placeholder and any "model" key that merely
                    // appears inside quoted content.
                    if (line.find("\"type\":\"assistant\"") == std::string_view::npos) return;
                    std::string_view model = extractModel(line);
                    if (!model.empty()) models[transcriptId] = std::string(model);
                });
            }
        }
    }

    result.reserve(sessions.size());
    for (const auto& [sid, info] : sessions) {
        std::string title;
        auto tit = titles.find(sid);
        if (tit != titles.end()) title = tit->second;
        else title = truncateUtf8(info.prompt, 80);
        if (title.empty()) title = "(no title)";

        // Claude Code prunes transcripts after cleanupPeriodDays (30 by
        // default), so most older sessions have no file to measure. Mark them
        // as unknown rather than leaving the cells blank.
        long long size = 0;
        std::string model = "\xE2\x80\x94";  // em dash
        auto sz = sizes.find(sid);
        bool hasTranscript = sz != sizes.end();
        if (hasTranscript) size = sz->second;
        if (auto md = models.find(sid); md != models.end()) model = md->second;

        result.push_back(Session{
            "Claude", sid, info.dir, title, std::move(model),
            info.created, info.updated, size, hasTranscript });
    }
    return result;
}

std::vector<Session> SessionLoader::loadOpenCode() {
    // %LOCALAPPDATA%\share\opencode\opencode.db is where the Windows build
    // keeps its database; the ~/.local/share layout is the fallback.
    fs::path primary = localAppData() / L"share" / L"opencode" / L"opencode.db";
    fs::path fallback = homePath() / L".local" / L"share" / L"opencode" / L"opencode.db";

    std::error_code ec;
    fs::path path;
    if (fs::exists(primary, ec)) path = primary;
    else if (fs::exists(fallback, ec)) path = fallback;
    if (path.empty()) return {};

    std::vector<Session> result;
    sqlite3* db = nullptr;
    // sqlite3 takes UTF-8 paths on Windows.
    std::string utf8Path = utf16to8(path.wstring());
    if (sqlite3_open_v2(utf8Path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return {};
    }

    // A session's content lives in the message and part tables as JSON text;
    // its size is the sum of both, in bytes. Two aggregate queries up front
    // are far cheaper than one per session.
    std::unordered_map<std::string, long long> sizes;
    for (const char* table : {"message", "part"}) {
        std::string aggregate = std::string(
            "SELECT session_id, SUM(LENGTH(CAST(data AS BLOB))) FROM ") + table +
            " GROUP BY session_id";
        sqlite3_stmt* agg = nullptr;
        if (sqlite3_prepare_v2(db, aggregate.c_str(), -1, &agg, nullptr) != SQLITE_OK) continue;
        while (sqlite3_step(agg) == SQLITE_ROW) {
            const unsigned char* id = sqlite3_column_text(agg, 0);
            if (id) sizes[reinterpret_cast<const char*>(id)] += sqlite3_column_int64(agg, 1);
        }
        sqlite3_finalize(agg);
    }

    const char* sql =
        "SELECT id, directory, title, time_created, time_updated, model "
        "FROM session ORDER BY time_updated DESC";
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        sqlite3_close(db);
        return {};
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        auto col = [&](int i) -> std::string {
            const unsigned char* t = sqlite3_column_text(stmt, i);
            return t ? std::string(reinterpret_cast<const char*>(t)) : std::string{};
        };

        std::string modelRaw = col(5);
        std::string modelStr;
        if (!modelRaw.empty()) {
            mini::JValue md;
            std::string id;
            if (mini::parse(modelRaw, md) && md.isObject()) id = md.strOf("id");
            modelStr = id.empty() ? modelRaw : id;
        }

        std::string title = col(2);
        if (title.empty()) title = "(no title)";

        std::string id = col(0);
        long long size = 0;
        if (auto sz = sizes.find(id); sz != sizes.end()) size = sz->second;

        result.push_back(Session{
            "OpenCode",
            std::move(id),
            col(1),
            std::move(title),
            std::move(modelStr),
            sqlite3_column_int64(stmt, 3),
            sqlite3_column_int64(stmt, 4),
            size,
        });
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}
