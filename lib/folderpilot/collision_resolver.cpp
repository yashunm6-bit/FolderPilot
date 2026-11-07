// FolderPilot - planning-time collision resolution implementation.

#include "folderpilot/collision_resolver.hpp"

#include <array>
#include <utility>

#include "folderpilot/rules.hpp"
#include "native.hpp" // platform::compare_ordinal_ci (core/platform seam)

namespace folderpilot {

namespace {

// ASCII-only fold for compound-suffix detection; byte positions are
// preserved, so the original spelling can be sliced afterwards.
std::string ascii_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
    }
    return out;
}

} // namespace

NameParts split_destination_name(std::string_view filename) {
    const std::string folded = ascii_lower(filename);

    // Compound archives first: the whole compound suffix is the extension.
    for (const std::string_view suffix : compound_archive_suffixes()) {
        if (folded.size() > suffix.size() && folded.ends_with(suffix)) {
            return {std::string{filename.substr(0, folded.size() - suffix.size())},
                    std::string{filename.substr(folded.size() - suffix.size())}};
        }
    }

    const std::size_t last_dot = folded.rfind('.');
    if (last_dot == std::string::npos || last_dot == 0) {
        // No extension, or a dotfile (leading dot is not an extension).
        return {std::string{filename}, {}};
    }
    return {std::string{filename.substr(0, last_dot)},
            std::string{filename.substr(last_dot)}};
}

std::string suffixed_name(const NameParts& parts, std::size_t n) {
    return parts.stem + " (" + std::to_string(n) + ")" + parts.extension;
}

bool CollisionResolver::CiLess::operator()(const std::string& a, const std::string& b) const {
    return platform::compare_ordinal_ci(a, b) < 0;
}

CollisionResolver::CollisionResolver(std::size_t max_suffix_attempts)
    : max_suffix_attempts_{max_suffix_attempts} {}

void CollisionResolver::add_existing(std::string_view name) {
    existing_.insert(std::string{name});
}

void CollisionResolver::reserve_natural(std::string_view name) {
    wanted_.insert(std::string{name});
}

std::optional<std::string> CollisionResolver::allocate(std::string_view natural_name) {
    // A candidate is free when it collides with neither the destination
    // snapshot nor an earlier allocation, and (for generated names) steals
    // no incoming file's reserved natural name. The candidate's OWN natural
    // name is exempt from the reservation check.
    const std::string natural{natural_name};
    const auto is_free = [&](const std::string& candidate) {
        return !existing_.contains(candidate) && !claimed_.contains(candidate) &&
               (candidate == natural || !wanted_.contains(candidate));
    };

    if (is_free(natural)) {
        claimed_.insert(natural);
        return natural;
    }

    const NameParts parts = split_destination_name(natural);
    for (std::size_t attempt = 2; attempt <= max_suffix_attempts_ + 1; ++attempt) {
        std::string candidate = suffixed_name(parts, attempt);
        if (is_free(candidate)) {
            claimed_.insert(candidate);
            return candidate;
        }
    }
    return std::nullopt; // CollisionExhausted
}

} // namespace folderpilot
