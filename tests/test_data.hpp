// FolderPilot - shared test data.
//
// The documented Phase 2 extension lists (the normative rule set). Used by
// both test_rules.cpp (rules must contain exactly these) and
// test_classifier.cpp (every listed extension must classify correctly), so
// the two test files can never drift apart.

#pragma once

#include <array>
#include <string_view>

#include "folderpilot/category.hpp"

namespace folderpilot_test_data {

inline constexpr std::array<std::string_view, 15> documents{
    "pdf", "doc", "docx", "txt", "rtf", "md", "odt", "xls",
    "xlsx", "ppt", "pptx", "csv", "epub", "one", "xps",
};

inline constexpr std::array<std::string_view, 14> images{
    "jpg", "jpeg", "png", "gif", "bmp", "webp", "tif", "tiff",
    "heic", "heif", "svg", "ico", "raw", "psd",
};

// NOTE on ".ts": the normative rule lists contained "ts" under BOTH Videos
// and Code, but every extension must map to exactly one category. Resolved
// to Code (TypeScript, beside "js"); MPEG transport streams are the rare
// real-world case. Videos therefore documents 11 extensions.
inline constexpr std::array<std::string_view, 11> videos{
    "mp4", "mkv", "avi", "mov", "wmv", "flv",
    "webm", "m4v", "mpg", "mpeg", "3gp",
};

inline constexpr std::array<std::string_view, 11> audio{
    "mp3", "wav", "flac", "aac", "ogg", "oga",
    "m4a", "wma", "opus", "mid", "midi",
};

inline constexpr std::array<std::string_view, 11> archives{
    "zip", "rar", "7z", "tar", "gz", "bz2",
    "xz", "zst", "iso", "cab", "tgz",
};

inline constexpr std::array<std::string_view, 27> code{
    "c", "cc", "cpp", "h", "hpp", "py", "java", "js", "ts",
    "html", "css", "json", "xml", "cs", "rb", "go", "rs", "php",
    "sh", "bat", "ps1", "sql", "yml", "yaml", "toml", "ini", "lua",
};

// Total number of documented rules (no category has a static rule table
// entry for Other - it is the fallback).
inline constexpr std::size_t total_documented_rules =
    documents.size() + images.size() + videos.size() +
    audio.size() + archives.size() + code.size();

} // namespace folderpilot_test_data
