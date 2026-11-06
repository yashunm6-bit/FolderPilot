// FolderPilot - pure filename classification.
//
// IMPORTANT CONTRACT: classify_filename() takes a file BASENAME (the file's
// name without any directory part), encoded in UTF-8. It never touches the
// filesystem: no opening, no reading, no metadata, no directories. It is a
// pure string function and therefore trivially testable in memory.

#pragma once

#include <string_view>

#include "folderpilot/category.hpp"

namespace folderpilot {

// Classifies a UTF-8 filename basename by its extension.
//
// Rules, in order:
//   1. Compound archive suffixes (.tar.gz / .tar.bz2 / .tar.xz) map to
//      Archives before ordinary extension lookup.
//   2. Otherwise the extension after the LAST dot is looked up
//      ASCII case-insensitively.
//   3. Anything that has no usable extension - dotfiles such as
//      ".gitignore" or ".pdf", names ending in a dot, unknown extensions,
//      and empty input - maps to Category::Other.
//
// As a defensive measure, if a full path is passed by mistake, everything
// before the last '/' or '\\' is ignored; the basename is classified.
Category classify_filename(std::string_view filename);

} // namespace folderpilot
