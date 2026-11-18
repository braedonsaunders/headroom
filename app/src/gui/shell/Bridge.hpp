#pragma once
// Page → native actions (the StudioApp.mm protocol). Thread-safe `eval` is provided by the shell.
#include "Shell.hpp"
#include <functional>
#include <string>

namespace rig::shell {

class Bridge {
public:
    using EvalFn = std::function<void(std::string js)>; // must be callable from any thread
    Bridge(Platform& p, EvalFn eval) : platform_(p), eval_(std::move(eval)) {}
    // One message as JSON ({"action": ...}). Runs on the main thread.
    void handle(const std::string& json);

private:
    Platform& platform_;
    EvalFn eval_;
};

// JS injected at document start (before the UI's own scripts).
std::string bridgeInitScript(unsigned port, bool screenshot);

} // namespace rig::shell
