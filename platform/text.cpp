// FolderPilot - platform text conversion implementation.
//
// Conversion pattern for both directions (the standard Win32 idiom):
//   1. Call the API once with an empty output buffer to get the required
//      size (the returned count INCLUDES the null terminator it appends).
//   2. Allocate exactly that much and call again with the real buffer.
//   3. Shrink by one to drop the terminator the API appended.

#include "text.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <limits>
#include <utility>

namespace folderpilot::platform {

namespace {

// The Win32 conversion APIs take int lengths. Guard the size_t -> int
// narrowing explicitly instead of letting it wrap silently.
constexpr std::size_t kMaxLength =
    static_cast<std::size_t>((std::numeric_limits<int>::max)());

std::string api_error(const char* apiName) {
    return std::string(apiName) + " failed (Windows error " +
           std::to_string(::GetLastError()) + ")";
}

} // namespace

Utf16ConversionResult to_utf16(std::string_view utf8) {
    if (utf8.empty()) {
        return {true, {}, {}};
    }
    if (utf8.size() > kMaxLength) {
        return {false, {}, "UTF-8 input too long for conversion (exceeds INT_MAX code units)"};
    }
    const int length = static_cast<int>(utf8.size());

    const int required = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, nullptr, 0);
    if (required <= 0) {
        return {false, {}, api_error("MultiByteToWideChar")};
    }

    // With an EXPLICIT source length (not the cchSrc = -1 null-terminated
    // idiom), the API neither appends nor counts a null terminator: the size
    // query and the conversion both return exactly the converted count.
    std::wstring out(static_cast<std::size_t>(required), L'\0');
    const int written = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, out.data(), required);
    if (written != required) {
        return {false, {}, api_error("MultiByteToWideChar")};
    }

    return {true, std::move(out), {}};
}

Utf8ConversionResult to_utf8(std::wstring_view utf16) {
    if (utf16.empty()) {
        return {true, {}, {}};
    }
    if (utf16.size() > kMaxLength) {
        return {false, {}, "UTF-16 input too long for conversion (exceeds INT_MAX code units)"};
    }
    const int length = static_cast<int>(utf16.size());

    // For CP_UTF8 the default-char out-parameter must be null; strictness
    // comes from WC_ERR_INVALID_CHARS alone. As above: explicit length means
    // no null terminator is appended or counted.
    const int required = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(), length,
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return {false, {}, api_error("WideCharToMultiByte")};
    }

    std::string out(static_cast<std::size_t>(required), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, utf16.data(), length,
        out.data(), required, nullptr, nullptr);
    if (written != required) {
        return {false, {}, api_error("WideCharToMultiByte")};
    }

    return {true, std::move(out), {}};
}

} // namespace folderpilot::platform
