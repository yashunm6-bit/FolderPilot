// FolderPilot - core category type implementation.

#include "folderpilot/category.hpp"

namespace folderpilot {

std::string_view category_name(Category category) {
    switch (category) {
        case Category::Documents: return "Documents";
        case Category::Images:    return "Images";
        case Category::Videos:    return "Videos";
        case Category::Audio:     return "Audio";
        case Category::Archives:  return "Archives";
        case Category::Code:      return "Code";
        case Category::Other:     return "Other";
    }
    // Any value outside the enum's valid range lands here (the switch covers
    // every declared enumerator, so the compiler warns if one is missing).
    return "Unknown";
}

} // namespace folderpilot
