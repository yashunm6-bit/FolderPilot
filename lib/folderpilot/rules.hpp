// FolderPilot - file extension classification rules.
//
// This file owns the extension-to-category DATA only. The logic that uses it
// lives in the classifier. The table is static, sorted, and searched with
// binary search so classification is deterministic and allocation-free.

#pragma once

#include <optional>
#include <span>
#include <string_view>

#include "folderpilot/category.hpp"

namespace folderpilot {

// One rule: an extension (lower-case, WITHOUT the leading dot) and the
// category it maps to.
struct ExtensionRule {
    std::string_view extension;
    Category category;
};

// Looks up an extension, ASCII case-insensitively (so "PDF", "pdf" and "Pdf"
// all match). The input must be the bare extension without the leading dot.
// Returns std::nullopt for unknown extensions.
std::optional<Category> lookup_extension(std::string_view extension);

// The full static rule table, sorted by extension. Exposed read-only so that
// tests can verify sorting, uniqueness and completeness.
std::span<const ExtensionRule> extension_rules();

// Compound archive suffixes (INCLUDING the leading dot) that are recognized
// before ordinary single-extension lookup: "photos.tar.gz" must become an
// Archive, not be classified by its final "gz" alone. Case-insensitive.
std::span<const std::string_view> compound_archive_suffixes();

} // namespace folderpilot
