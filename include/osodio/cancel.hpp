#pragma once
#include <atomic>
#include <memory>

namespace osodio {

// ─── CancellationToken ────────────────────────────────────────────────────────
//
// Shared between an HttpConnection and every Request it spawns.
// When the connection closes (timeout, client disconnect, write error),
// the token is cancelled.  Handlers can check it to exit early:
//
//   app.get("/poll", [](Request& req) -> Task<json> {
//       while (true) {
//           co_await sleep(1000);
//           if (req.is_cancelled()) co_return {};   // connection gone
//           // ... do work ...
//       }
//   });
//
// SleepAwaitable checks the token on resume: if cancelled, the coroutine is
// resumed immediately (returns from the sleep) so the caller can check and
// exit, rather than waiting for the full timer duration.

struct CancellationToken {
    std::atomic<bool> cancelled{false};

    void cancel() noexcept {
        cancelled.store(true, std::memory_order_release);
    }

    bool is_cancelled() const noexcept {
        return cancelled.load(std::memory_order_acquire);
    }
};

} // namespace osodio
