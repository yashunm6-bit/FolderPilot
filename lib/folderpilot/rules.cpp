// FolderPilot - file extension classification rules implementation.
//
// The table below is the single source of truth for extension mapping.
// Invariants (enforced by tests/unit/test_rules.cpp):
//   * sorted strictly increasing by extension  =>  no duplicates,
//   * every extension maps to exactly one category,
//   * every category except Other (the fallback) is represented.

#include "folderpilot/rules.hpp"

#include <algorithm>

namespace folderpilot {

namespace {

// Sorted, lower-case, dot-less. Keep it that way - lookup_extension relies
// on the order for binary search, and tests enforce it.
constexpr ExtensionRule kRules[] = {
    // Videos
    {"3gp", Category::Videos},
    // Archives
    {"7z", Category::Archives},
    // Audio
    {"aac", Category::Audio},
    // Videos
    {"avi", Category::Videos},
    // Code / Images / Archives
    {"bat", Category::Code},
    {"bmp", Category::Images},
    {"bz2", Category::Archives},
    // Code / Archives
    {"c", Category::Code},
    {"cab", Category::Archives},
    {"cc", Category::Code},
    {"cpp", Category::Code},
    {"cs", Category::Code},
    {"css", Category::Code},
    // Documents
    {"csv", Category::Documents},
    {"doc", Category::Documents},
    {"docx", Category::Documents},
    {"epub", Category::Documents},
    // Audio / Videos
    {"flac", Category::Audio},
    {"flv", Category::Videos},
    // Images / Code / Archives
    {"gif", Category::Images},
    {"go", Category::Code},
    {"gz", Category::Archives},
    // Code / Images
    {"h", Category::Code},
    {"heic", Category::Images},
    {"heif", Category::Images},
    {"hpp", Category::Code},
    {"html", Category::Code},
    // Images / Code / Archives
    {"ico", Category::Images},
    {"ini", Category::Code},
    {"iso", Category::Archives},
    // Code / Images
    {"java", Category::Code},
    {"jpeg", Category::Images},
    {"jpg", Category::Images},
    {"js", Category::Code},
    {"json", Category::Code},
    // Code
    {"lua", Category::Code},
    // Audio / Videos / Documents
    {"m4a", Category::Audio},
    {"m4v", Category::Videos},
    {"md", Category::Documents},
    {"mid", Category::Audio},
    {"midi", Category::Audio},
    {"mkv", Category::Videos},
    {"mov", Category::Videos},
    {"mp3", Category::Audio},
    {"mp4", Category::Videos},
    {"mpeg", Category::Videos},
    {"mpg", Category::Videos},
    // Documents / Audio
    {"odt", Category::Documents},
    {"oga", Category::Audio},
    {"ogg", Category::Audio},
    {"one", Category::Documents},
    {"opus", Category::Audio},
    // Documents / Code / Images
    {"pdf", Category::Documents},
    {"php", Category::Code},
    {"png", Category::Images},
    {"ppt", Category::Documents},
    {"pptx", Category::Documents},
    {"ps1", Category::Code},
    {"psd", Category::Images},
    {"py", Category::Code},
    // Archives / Images / Code / Documents
    {"rar", Category::Archives},
    {"raw", Category::Images},
    {"rb", Category::Code},
    {"rs", Category::Code},
    {"rtf", Category::Documents},
    // Code / Images
    {"sh", Category::Code},
    {"sql", Category::Code},
    {"svg", Category::Images},
    // Archives / Images / Code / Documents
    {"tar", Category::Archives},
    {"tgz", Category::Archives},
    {"tif", Category::Images},
    {"tiff", Category::Images},
    {"toml", Category::Code},
    {"ts", Category::Code},
    {"txt", Category::Documents},
    // Audio / Videos / Images / Documents
    {"wav", Category::Audio},
    {"webm", Category::Videos},
    {"webp", Category::Images},
    {"wma", Category::Audio},
    {"wmv", Category::Videos},
    // Documents / Code
    {"xls", Category::Documents},
    {"xlsx", Category::Documents},
    {"xml", Category::Code},
    {"xps", Category::Documents},
    {"xz", Category::Archives},
    // Code / Archives
    {"yaml", Category::Code},
    {"yml", Category::Code},
    {"zip", Category::Archives},
    {"zst", Category::Archives},
};

// ASCII-only case fold: 'A'..'Z' become 'a'..'z'. Bytes >= 0x80 (every byte
// of a multi-byte UTF-8 sequence) are never touched, so UTF-8 content is
// always preserved byte-for-byte.
constexpr char ascii_fold(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Case-insensitive comparison used by the binary search. Folding both sides
// of every comparison is correct here because the table itself is stored
// lower-case, so folding cannot change the table's relative order.
int compare_ascii_ci(std::string_view a, std::string_view b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const unsigned char ca = static_cast<unsigned char>(ascii_fold(a[i]));
        const unsigned char cb = static_cast<unsigned char>(ascii_fold(b[i]));
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
    }
    if (a.size() == b.size()) {
        return 0;
    }
    return a.size() < b.size() ? -1 : 1;
}

constexpr std::string_view kCompoundArchiveSuffixes[] = {
    ".tar.gz",
    ".tar.bz2",
    ".tar.xz",
};

} // namespace

std::optional<Category> lookup_extension(std::string_view extension) {
    if (extension.empty()) {
        return std::nullopt;
    }

    // Classic binary search over the sorted table.
    std::size_t low = 0;
    std::size_t high = std::size(kRules);
    while (low < high) {
        const std::size_t mid = low + (high - low) / 2;
        const int cmp = compare_ascii_ci(extension, kRules[mid].extension);
        if (cmp == 0) {
            return kRules[mid].category;
        }
        if (cmp < 0) {
            high = mid;
        } else {
            low = mid + 1;
        }
    }
    return std::nullopt;
}

std::span<const ExtensionRule> extension_rules() {
    return kRules;
}

std::span<const std::string_view> compound_archive_suffixes() {
    return kCompoundArchiveSuffixes;
}

} // namespace folderpilot
