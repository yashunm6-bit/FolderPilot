// FolderPilot - PathGuard implementation.

#include "folderpilot/path_guard.hpp"

#include <array>

#include "native.hpp"

namespace folderpilot {

namespace {

// Denied system subtrees: the component itself and everything beneath it.
constexpr std::array<std::string_view, 8> kSystemSubtrees{
    "Windows",
    "Program Files",
    "Program Files (x86)",
    "ProgramData",
    "Boot",
    "Recovery",
    "System Volume Information",
    "$Recycle.Bin",
};

PathGuardResult rejection(PathRejection reason, std::string message,
                          unsigned long native_code = 0) {
    PathGuardResult result;
    result.ok = false;
    result.rejection = reason;
    result.error = std::move(message);
    result.native_code = native_code;
    return result; // root handle (if any was opened) closes via RAII
}

PathRejection map_platform_failure(platform::NativeFailure failure) {
    switch (failure) {
        case platform::NativeFailure::ConfirmedNotFound: return PathRejection::MissingTarget;
        case platform::NativeFailure::InvalidPath:       return PathRejection::InvalidPath;
        case platform::NativeFailure::NotADirectory:     return PathRejection::NotADirectory;
        case platform::NativeFailure::AccessDenied:      return PathRejection::AccessDenied;
        case platform::NativeFailure::SharingConflict:   return PathRejection::SharingConflict;
        case platform::NativeFailure::DeletePending:     return PathRejection::SharingConflict;
        case platform::NativeFailure::Unsupported:       return PathRejection::UnsupportedPlatform;
        case platform::NativeFailure::IsADirectory:      return PathRejection::NotADirectory;
        case platform::NativeFailure::Conflict:          return PathRejection::OtherError;
        case platform::NativeFailure::CrossVolume:       return PathRejection::OtherError;
        case platform::NativeFailure::Other:             return PathRejection::OtherError;
    }
    return PathRejection::OtherError;
}

} // namespace

std::vector<std::string> split_path_components(std::string_view path) {
    std::vector<std::string> components;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t sep = path.find('\\', start);
        const std::string_view part =
            (sep == std::string_view::npos) ? path.substr(start) : path.substr(start, sep - start);
        if (!part.empty()) {
            components.emplace_back(part);
        }
        if (sep == std::string_view::npos) {
            break;
        }
        start = sep + 1;
    }
    return components;
}

std::optional<std::pair<PathRejection, std::string>> evaluate_denylist(
    const std::vector<std::string>& components,
    const std::string& resolved_path,
    const std::optional<std::string>& home_path) {

    if (components.size() == 1) {
        // The only component is the drive ("C:") - that is a drive root.
        return std::make_pair(PathRejection::DriveRoot,
                              "target is a drive root: " + resolved_path);
    }

    for (std::size_t i = 1; i < components.size(); ++i) { // index 0 is the drive
        const std::string& component = components[i];

        for (const std::string_view denied : kSystemSubtrees) {
            if (platform::compare_ordinal_ci(component, denied) == 0) {
                return std::make_pair(PathRejection::SystemSubtree,
                                      "target is under a denied system subtree: " + std::string{denied});
            }
        }

        if (platform::compare_ordinal_ci(component, "Users") == 0) {
            if (i + 1 == components.size()) {
                return std::make_pair(PathRejection::UsersDepthPolicy,
                                      "target is the Users directory itself");
            }
            if (i + 2 == components.size()) {
                return std::make_pair(PathRejection::UsersDepthPolicy,
                                      "target is a user profile root (direct child of Users): " +
                                          resolved_path);
            }
            // Deeper directories are allowed on purpose.
        }
    }

    if (home_path.has_value() &&
        platform::compare_ordinal_ci(resolved_path, *home_path) == 0) {
        return std::make_pair(PathRejection::HomeRoot,
                              "target is the current user's home directory itself");
    }

    return std::nullopt;
}

PathGuardResult validate_target(std::string_view utf8_path) {
    // ---- 1. Bootstrap open (the one path-based operation; platform layer).
    platform::RootOpenResult opened = platform::open_root(utf8_path);
    if (!opened.ok) {
        return rejection(map_platform_failure(opened.failure), opened.error, opened.native_code);
    }

    // ---- 2. Attribute gate on the actual opened handle (source of truth).
    const platform::EntryAttributes& attrs = opened.root.attributes;
    if (!attrs.directory()) {
        return rejection(PathRejection::NotADirectory, "target is not a directory");
    }
    if (attrs.reparse_point()) {
        return rejection(PathRejection::ReparseLeafDirectory,
                         "target is a reparse point (junction, symlink or cloud placeholder)");
    }
    if (attrs.hidden()) {
        return rejection(PathRejection::HiddenDirectory, "target is a hidden directory");
    }
    if (attrs.system()) {
        return rejection(PathRejection::SystemDirectory, "target is a system directory");
    }

    // ---- 3. Component-aware denylist over the RESOLVED path.
    const std::vector<std::string> components = split_path_components(opened.root.resolved_path_utf8);
    if (components.empty()) {
        return rejection(PathRejection::PathResolutionFailed,
                         "resolved path could not be split into components");
    }
    const auto denied = evaluate_denylist(components, opened.root.resolved_path_utf8,
                                          platform::current_user_home());
    if (denied.has_value()) {
        return rejection(denied->first, denied->second);
    }

    // ---- 4. Success: hand over the retained handle and verified payload.
    PathGuardResult result;
    result.ok = true;
    result.requested_path = opened.root.requested_path_utf8;
    result.resolved_path = opened.root.resolved_path_utf8;
    result.attributes = opened.root.attributes;
    result.identity = opened.root.identity;
    result.volume = opened.root.volume;
    result.root = std::move(opened.root);
    return result;
}

} // namespace folderpilot
