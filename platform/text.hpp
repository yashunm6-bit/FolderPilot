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
