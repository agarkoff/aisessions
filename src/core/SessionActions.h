#pragma once
#include <string>

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

} // namespace SessionActions
