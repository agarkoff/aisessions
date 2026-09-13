#pragma once
#include <string>
#include <vector>

#include "Session.h"

// Actions the list's context menu performs on a single session.
namespace SessionActions {

// Opens a terminal in the session's directory and resumes it there:
// `claude --resume <id>` or `opencode --session <id>`. Uses Windows Terminal
// when it is installed and falls back to a console window.
bool resumeInTerminal(const Session& session, std::wstring& error);

// Removes the session for good.
//
// OpenCode goes through `opencode session delete`, which is the vendor's own
// path and keeps the database consistent while opencode is running. Claude has
// no such command, so its transcript is moved to the Recycle Bin - recoverable
// - and its entries are dropped from history.jsonl, which is rewritten
// atomically with the original kept as history.jsonl.bak.
bool deleteSession(const Session& session, std::wstring& error);

// Removes several Claude sessions with a single rewrite of history.jsonl.
// Meant for the stale ones - those with no transcript left - where a
// per-session rewrite of a multi-megabyte file would add up. `freedBytes`
// receives how much smaller history.jsonl got plus the size of any transcript
// that was moved to the Recycle Bin.
bool deleteClaudeSessions(const std::vector<std::string>& sessionIds,
                          long long& freedBytes, std::wstring& error);

// Starts a brand-new session of the given agent ("Claude" or "OpenCode") in
// `directory` - a bare `claude` or `opencode`, no --resume/--session flag.
// Used from the folder tree's "New session here" context menu.
bool startNewSession(const std::string& agent, const std::wstring& directory,
                     std::wstring& error);

} // namespace SessionActions
