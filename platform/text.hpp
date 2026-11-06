// FolderPilot - platform text conversion (Windows UTF-8 / UTF-16).
//
// Windows filesystem paths are natively UTF-16; FolderPilot's internal text
// is UTF-8. This component converts between the two using the Win32
// conversion APIs with STRICT validation:
//   * UTF-8  -> UTF-16: MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, ...)
//   * UTF-16 -> UTF-8:  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, ...)
// Invalid input is rejected with an explicit error - characters are never
// silently replaced, dropped, or "best-fit" translated. The Windows ANSI
// code page is never used.

#pragma once

#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace folderpilot::platform {

// Result of converting UTF-8 text to UTF-16 (std::wstring on Windows).
// Beginner-friendly shape: check `ok` first; on success `value` holds the
// converted text, on failure `error` explains what went wrong.
struct Utf16ConversionResult {
    bool ok = false;
    std::wstring value;
    std::string error;
};

// Result of converting UTF-16 text to UTF-8 (same shape, narrow output).
struct Utf8ConversionResult {
    bool ok = false;
    std::string value;
    std::string error;
};

// Largest number of code units a single Win32 conversion call can accept:
// the Win32 APIs take int lengths, and INT_MAX is their upper bound.
inline constexpr std::size_t kMaxConversionLength =
    static_cast<std::size_t>((std::numeric_limits<int>::max)());

// Converts a buffer size (in code units) to the int length that the Win32
// conversion APIs accept. Returns std::nullopt when `size` exceeds INT_MAX,
// because such a buffer cannot be passed to Win32 in a single call.
// Pure and allocation-free; exposed so the boundary can be unit-tested
// without allocating multi-gigabyte strings.
std::optional<int> to_win32_length(std::size_t size);

// Converts UTF-8 input (explicit length via the string_view) to UTF-16.
// Empty input converts successfully to an empty result.
// Fails on any invalid UTF-8 sequence, including truncated sequences,
// overlong encodings, and UTF-8-encoded surrogates.
Utf16ConversionResult to_utf16(std::string_view utf8);

// Converts UTF-16 input (explicit length via the wstring_view) to UTF-8.
// Empty input converts successfully to an empty result.
// Fails on any invalid UTF-16, including unpaired surrogates.
Utf8ConversionResult to_utf8(std::wstring_view utf16);

} // namespace folderpilot::platform
