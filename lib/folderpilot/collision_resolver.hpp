// FolderPilot - planning-time collision resolution.
//
// Pure decision logic: given the destination snapshot (names that already
// exist) and the incoming candidate names, it allocates a final destination
// filename for every incoming file. All matching is ordinal case-insensitive
// (conservative on purpose: a false-positive collision may add a suffix, it
// can never cause an overwrite). Original spelling is always preserved.

#pragma once

#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace folderpilot {

// Splits a filename into {stem, extension} for suffix generation.
//   * compound archives keep the whole compound extension:
//       "backup.tar.gz"     -> stem "backup",     extension ".tar.gz"
//   * ordinary files split at the LAST dot:
//       "report.pdf"        -> stem "report",     extension ".pdf"
//   * a trailing dot belongs to the extension:
//       "file."             -> stem "file",       extension "."
//   * dotfiles have NO extension (a leading dot is not an extension):
//       ".gitignore"        -> stem ".gitignore", extension ""
//   * extensionless files:
//       "README"            -> stem "README",     extension ""
struct NameParts {
    std::string stem;       // may itself contain dots
    std::string extension;  // includes the leading dot; "" when none
};
NameParts split_destination_name(std::string_view filename);

// Builds "stem (n)extension" (e.g. n == 2 -> "report (2).pdf").
std::string suffixed_name(const NameParts& parts, std::size_t n);

// Allocates destination names for one destination category.
// Usage contract (enforced by the planner):
//   1. add_existing() for every name in the destination snapshot,
//   2. reserve_natural() for EVERY incoming candidate's natural name,
//   3. allocate() for each candidate in deterministic order.
// Pre-reserving natural names guarantees that a generated suffix can never
// steal another incoming file's natural destination.
class CollisionResolver {
public:
    explicit CollisionResolver(std::size_t max_suffix_attempts = 100);

    void add_existing(std::string_view name);
    void reserve_natural(std::string_view name);

    // Returns the allocated destination name, or std::nullopt when no free
    // name was found within the suffix limit (CollisionExhausted).
    std::optional<std::string> allocate(std::string_view natural_name);

    std::size_t max_suffix_attempts() const { return max_suffix_attempts_; }

private:
    struct CiLess {
        bool operator()(const std::string& a, const std::string& b) const;
    };

    std::set<std::string, CiLess> existing_;  // destination snapshot
    std::set<std::string, CiLess> wanted_;    // natural names of all incoming files
    std::set<std::string, CiLess> claimed_;   // final allocations so far
    std::size_t max_suffix_attempts_;
};

} // namespace folderpilot
