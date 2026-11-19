// FolderPilot - dry-run reporter.
//
// Consumes ONLY an OrganizationPlan and renders it as text. It never
// inspects the filesystem, never generates or changes a destination name,
// never re-classifies, never re-plans, and never touches the plan.

#pragma once

#include <string>

#include "folderpilot/executor.hpp"
#include "folderpilot/organization_plan.hpp"
#include "folderpilot/undo_execute.hpp"
#include "folderpilot/undo_plan.hpp"

namespace folderpilot {

// Renders the read-only dry-run preview of the given plan.
// (ASCII arrow "->" is used on purpose: console encoding is finalized in a
// later phase, and the report must never depend on it.)
std::string render_dry_run_report(const OrganizationPlan& plan);

// Phase 7.3: undo reporting (preview and execution). Both are render-only.
std::string render_undo_preview(const UndoPlanResult& plan);
std::string render_undo_execution(const UndoPlanResult& plan,
                                  const UndoExecutionResult& result);

// Renders the execution report for a completed (or aborted) run. Consumes
// the immutable plan and the separate ExecutionResult; states clearly that
// the batch is not transactional and never claims success when a file failed.
std::string render_execution_report(const OrganizationPlan& plan,
                                    const ExecutionResult& result);

} // namespace folderpilot
