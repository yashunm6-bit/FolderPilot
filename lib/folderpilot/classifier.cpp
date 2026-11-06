// FolderPilot - pure filename classification implementation.
//
// No filesystem access happens anywhere in this file. See classifier.hpp
// for the classification contract.

#include "folderpilot/classifier.hpp"

#include <string>

#include "folderpilot/rules.hpp"

namespace folderpilot {

namespace {

// If a caller passes a full path by mistake, classify only the basename.
// UTF-8 continuation bytes are always >= 0x80, so they can never be confused
// with the ASCII separators '/' or '\\'.
std::string_view basename_of(std::string_view filename) {
    const std::size_t pos = filename.find_last_of("/\\");
    return (pos == std::string_view::npos) ? filename : filename.substr(pos + 1);
}

// ASCII-only lowercasing into a fresh string. Bytes >= 0x80 (all multi-byte
// UTF-8 content) pass through unchanged. Deliberately NOT locale-dependent
// std::tolower over the whole string: that could mis-fold UTF-8 bytes.
std::string ascii_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

} // namespace

Category classify_filename(std::string_view filename) {
    // ASCII folding preserves byte positions (only 'A'..'Z' change), so the
    // folded copy can be used for both compound-suffix checks and for finding
    // the last dot without disturbing any UTF-8 content.
    const std::string folded = ascii_lower(basename_of(filename));

    if (folded.empty()) {
        return Category::Other;
    }

    // Compound archives first: "photos.tar.gz" is an Archive even though its
    // final extension is "gz". Requiring folded.size() > suffix.size() keeps
    // a file literally named ".tar.gz" on the dotfile path below.
    for (const std::string_view suffix : compound_archive_suffixes()) {
        if (folded.size() > suffix.size() && folded.ends_with(suffix)) {
            return Category::Archives;
        }
    }

    const std::size_t last_dot = folded.rfind('.');
    if (last_dot == std::string::npos || last_dot == 0) {
        // No dot at all, or a leading dot only (dotfile: ".gitignore", ".pdf").
        return Category::Other;
    }

    const std::string_view extension =
        std::string_view{folded}.substr(last_dot + 1);
    if (extension.empty()) {
        // Trailing dot: "file." has no usable extension.
        return Category::Other;
    }

    const std::optional<Category> category = lookup_extension(extension);
    return category ? *category : Category::Other;
}

} // namespace folderpilot
