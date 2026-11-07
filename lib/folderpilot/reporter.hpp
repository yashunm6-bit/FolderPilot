// FolderPilot - dry-run reporter.
//
// Consumes ONLY an OrganizationPlan and renders it as text. It never
// inspects the filesystem, never generates or changes a destination name,
// never re-classifies, never re-plans, and never touches the plan.

#pragma once

#include <string>

#include "folderpilot/organization_plan.hpp"

namespace folderpilot {

// Renders the read-only dry-run preview of the given plan.
// (ASCII arrow "->" is used on purpose: console encoding is finalized in a
// later phase, and the report must never depend on it.)
std::string render_dry_run_report(const OrganizationPlan& plan);

} // namespace folderpilot
