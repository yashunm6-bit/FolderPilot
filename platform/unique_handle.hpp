// FolderPilot - move-only RAII wrapper for a Win32 HANDLE.
//
// Guarantees:
//   * the owned handle (if any) is closed exactly once, on destruction or on
//     move-assignment overwrite,
//   * copying is impossible - handles can only be transferred by move,
//   * a moved-from wrapper is empty (valid() == false),
//   * no leaks on any path, including early returns and exceptions
//     (destruction is the only place that closes).

#pragma once

#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

namespace folderpilot::platform {

class unique_handle {
public:
    unique_handle() = default;
    explicit unique_handle(HANDLE handle) noexcept : handle_{handle} {}

    ~unique_handle() { reset(); }

    unique_handle(const unique_handle&) = delete;
    unique_handle& operator=(const unique_handle&) = delete;

    unique_handle(unique_handle&& other) noexcept
        : handle_{std::exchange(other.handle_, nullptr)} {}

    unique_handle& operator=(unique_handle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    bool valid() const noexcept { return handle_ != nullptr; }
    HANDLE get() const noexcept { return handle_; }

    // Gives up ownership without closing (rarely needed).
    HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

    // Closes the currently held handle (if any) and optionally adopts a new one.
    void reset(HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr) {
            ::CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_ = nullptr;
};

} // namespace folderpilot::platform
