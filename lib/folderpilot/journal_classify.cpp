// FolderPilot - Phase 7 move journal: reopened-state classification,
// cross-binding checks and capacity arithmetic.
//
// The forward classifier implements docs/PHASE7-DESIGN.md section 4.1 (the
// G8 classification table), section 6.3 (completeness states against
// planned_move_count), section 7 (END-record rules), and the W7/W8
// arithmetic of section 10, as pure functions over parsed bytes. Every
// ambiguity resolves blocked (fail-closed [SETTLED]). The segment
// classifier implements section 6.4 and the segment rows of section 10
// (segments are unanchored [G9]).
//
// Acknowledgement windows (docs section 8): activation happens at the first
// successfully anchored (covered) INTENT. A chain with at least one covered
// INTENT therefore reports Active even when a later record is ambiguous;
// Unknown is reserved for the first-INTENT-anchor-loss shape (no covered
// INTENT while INTENT evidence exists) and for evidence too broken to prove
// coverage.

#include "folderpilot/journal.hpp"

#include "sha256.hpp"

#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace folderpilot {

namespace {

JournalRecordType type_of(const JournalRecord& rec) {
    if (const auto* r = std::get_if<JournalRunStart>(&rec)) {
        (void)r;
        return JournalRecordType::RunStart;
    }
    if (std::get_if<JournalIntent>(&rec) != nullptr) return JournalRecordType::Intent;
    if (std::get_if<JournalResult>(&rec) != nullptr) return JournalRecordType::Result;
    if (std::get_if<JournalRunEnd>(&rec) != nullptr) return JournalRecordType::RunEnd;
    if (std::get_if<JournalUndoStart>(&rec) != nullptr) return JournalRecordType::UndoStart;
    if (std::get_if<JournalUndoIntent>(&rec) != nullptr) return JournalRecordType::UndoIntent;
    if (std::get_if<JournalUndoResult>(&rec) != nullptr) return JournalRecordType::UndoResult;
    return JournalRecordType::UndoEnd;
}

const JournalRunStart* head_run_start(const ParsedJournalFile& j) {
    if (j.records.empty()) {
        return nullptr;
    }
    return std::get_if<JournalRunStart>(&j.records[0]);
}

const JournalUndoStart* head_undo_start(const ParsedJournalFile& j) {
    if (j.records.empty()) {
        return nullptr;
    }
    return std::get_if<JournalUndoStart>(&j.records[0]);
}

std::size_t count_type(const std::vector<JournalRecord>& records, std::size_t end,
                       JournalRecordType t) {
    std::size_t n = 0;
    for (std::size_t i = 0; i < end && i < records.size(); ++i) {
        if (type_of(records[i]) == t) {
            ++n;
        }
    }
    return n;
}

std::size_t count_intent_evidence(const ParsedJournalFile& j) {
    std::size_t n = count_type(j.records, j.records.size(), JournalRecordType::Intent);
    if (j.tail == JournalTailKind::Torn && j.tail_type_hint == JournalRecordType::Intent) {
        ++n;
    }
    return n;
}

// Acknowledgement window status (docs section 8). covered_intents counts
// INTENTs within the provably anchored prefix; no_intent_anywhere marks a
// journal with zero INTENT records and no INTENT-shaped torn tail.
AckWindowStatus ack_status(bool has_ack, std::size_t covered_intents,
                           bool no_intent_anywhere) {
    if (!has_ack) {
        return AckWindowStatus::None;
    }
    if (covered_intents >= 1) {
        return AckWindowStatus::Active;
    }
    if (no_intent_anywhere) {
        return AckWindowStatus::Dormant;
    }
    return AckWindowStatus::Unknown;
}

} // namespace

ForwardChainClassification classify_forward_chain(const ParsedJournalFile& j,
                                                  const ParsedAnchorFile* anchor,
                                                  const ChainIdentityInputs& ids) {
    ForwardChainClassification out;

    const JournalRunStart* rs = head_run_start(j);
    const bool head_ack = rs != nullptr && rs->has_ack;

    // 1. Parse validity.
    if (!j.valid) {
        out.state = ForwardChainState::Blocked;
        out.reason = ForwardBlockReason::ParseInvalid;
        out.detail = j.failure.detail.empty() ? "journal parse failed" : j.failure.detail;
        // Evidence is broken; the acknowledgement pair may be unreadable.
        out.ack_window = head_ack ? AckWindowStatus::Unknown : AckWindowStatus::None;
        return out;
    }
    if (anchor != nullptr && !anchor->records.empty() && !anchor->valid) {
        out.state = ForwardChainState::Blocked;
        out.reason = ForwardBlockReason::ParseInvalid;
        out.detail = "anchor parse failed: " + anchor->failure.detail;
        out.ack_window = head_ack ? AckWindowStatus::Unknown : AckWindowStatus::None;
        return out;
    }
    const bool anchor_present = anchor != nullptr && !anchor->records.empty();

    // 2. Zero complete records (docs sections 4/6.3, G2): decided before the
    // RUN-START guard, because this journal shape has none. The ratified
    // fail-closed rule blocks a torn head that cannot reveal its record
    // type; only a head that uniquely identifies RUN-START (or a clean
    // empty file) is an abandoned forward initialization.
    if (j.records.empty()) {
        out.anchor_absent = !anchor_present;
        // An anchor file that exists but holds no complete record (torn
        // tail only) is disclosed as debris: nothing was ever anchored, so
        // the torn tail cannot hide coverage (T-70b's uncovered(J)-empty
        // arm). A non-empty anchor reaches the blocked coverage path below.
        if (!anchor_present && anchor != nullptr &&
            anchor->tail == JournalTailKind::Torn) {
            out.torn_anchor_debris = true;
        }
        if (anchor_present) {
            // R11 anchor-side binding first, then W7 (R11 precedes W7;
            // pinned by test). Every valid anchor record covers at least one
            // byte (the parser rejects a zero-byte genesis claim), so an
            // empty journal is always below coverage here.
            if (anchor->records[0].journal_id != ids.expected_journal_id) {
                out.state = ForwardChainState::Blocked;
                out.reason = ForwardBlockReason::IdentityMismatch;
                out.detail = "anchor names journal \"" + anchor->records[0].journal_id +
                             "\" but the storage-verified identity is \"" +
                             ids.expected_journal_id + "\"";
                return out;
            }
            const unsigned long long proven = anchor->records.back().byte_len;
            out.state = ForwardChainState::Blocked;
            out.reason = ForwardBlockReason::JournalBelowCoverage;
            out.detail = "journal has " + std::to_string(j.total_bytes) +
                         " bytes; the anchor proves " + std::to_string(proven) +
                         " bytes; lost range [" + std::to_string(j.total_bytes) + ", " +
                         std::to_string(proven) + ")";
            return out;
        }
        if (j.tail == JournalTailKind::Torn) {
            if (!j.tail_type_hint.has_value()) {
                out.state = ForwardChainState::Blocked;
                out.reason = ForwardBlockReason::UncoveredTailUntyped;
                out.detail = "torn tail is too short to reveal its record type";
                return out;
            }
            if (*j.tail_type_hint == JournalRecordType::RunStart) {
                out.state = ForwardChainState::AbandonedForwardInit;
                out.detail = "initialization abandoned with a torn head record";
                return out;
            }
            if (*j.tail_type_hint == JournalRecordType::Intent) {
                out.state = ForwardChainState::Blocked;
                out.reason = ForwardBlockReason::AnchorMissingWithIntents;
                out.detail = "INTENT evidence exists without any anchor record; a "
                             "previously authorized rename cannot be excluded";
                return out;
            }
            out.state = ForwardChainState::Blocked;
            out.reason = ForwardBlockReason::ProtocolViolation;
            out.detail = "torn tail of an impossible shape without any anchor record";
            return out;
        }
        out.state = ForwardChainState::AbandonedForwardInit;
        out.detail = "initialization abandoned with zero records";
        return out;
    }

    // 3. RUN-START head guard.
    if (rs == nullptr) {
        out.state = ForwardChainState::Blocked;
        out.reason = ForwardBlockReason::ParseInvalid;
        out.detail = "not a forward journal";
        return out;
    }

    // 4. Cross-binding (R11): the anchor names the journal, RUN-START names
    // the anchor. Mismatch on either side is a structured refusal.
    if (anchor_present) {
        if (anchor->records[0].journal_id != ids.expected_journal_id) {
            out.state = ForwardChainState::Blocked;
            out.reason = ForwardBlockReason::IdentityMismatch;
            out.detail = "anchor names journal \"" + anchor->records[0].journal_id +
                         "\" but the storage-verified identity is \"" +
                         ids.expected_journal_id + "\"";
            out.ack_window = head_ack ? AckWindowStatus::Unknown : AckWindowStatus::None;
            return out;
        }
        if (rs->anchor_id != ids.expected_anchor_id) {
            out.state = ForwardChainState::Blocked;
            out.reason = ForwardBlockReason::IdentityMismatch;
            out.detail = "RUN-START names anchor \"" + rs->anchor_id +
                         "\" but the storage-verified identity is \"" +
                         ids.expected_anchor_id + "\"";
            out.ack_window = head_ack ? AckWindowStatus::Unknown : AckWindowStatus::None;
            return out;
        }
    }

    // 5. Coverage claims versus the journal (W7, alignment, hash binding).
    const AnchorRecord* cov = nullptr;
    if (anchor_present) {
        cov = &anchor->records.back();
        if (cov->byte_len > j.total_bytes) {
            out.state = ForwardChainState::Blocked;
            out.reason = ForwardBlockReason::JournalBelowCoverage;
            out.detail = "journal has " + std::to_string(j.total_bytes) +
                         " bytes; the anchor proves " + std::to_string(cov->byte_len) +
                         " bytes; lost range [" + std::to_string(j.total_bytes) + ", " +
                         std::to_string(cov->byte_len) + ")";
            out.ack_window = head_ack ? AckWindowStatus::Unknown : AckWindowStatus::None;
            return out;
        }
        if (cov->covered_seq >= j.records.size()) {
            out.state = ForwardChainState::Blocked;
            out.reason = ForwardBlockReason::CorrelationAnomaly;
            out.detail = "anchor names journal record " + std::to_string(cov->covered_seq) +
                         " which does not exist (" + std::to_string(j.records.size()) +
                         " records present)";
            out.ack_window = head_ack ? AckWindowStatus::Unknown : AckWindowStatus::None;
            return out;
        }
        if (j.record_end[cov->covered_seq] != cov->byte_len) {
            out.state = ForwardChainState::Blocked;
            out.reason = ForwardBlockReason::CorrelationAnomaly;
            out.detail = "anchor byte_len does not align with a journal record boundary";
            out.ack_window = head_ack ? AckWindowStatus::Unknown : AckWindowStatus::None;
            return out;
        }
        if (type_of(j.records[cov->covered_seq]) == JournalRecordType::RunEnd) {
            out.state = ForwardChainState::Blocked;
            out.reason = ForwardBlockReason::ProtocolViolation;
            out.detail = "anchor claims coverage of RUN-END, which is never anchored";
            out.ack_window = head_ack ? AckWindowStatus::Unknown : AckWindowStatus::None;
            return out;
        }
        if (journal_hex(j.record_digests[cov->covered_seq]) != cov->last_record_hash) {
            out.state = ForwardChainState::Blocked;
            out.reason = ForwardBlockReason::CorrelationAnomaly;
            out.detail = "anchor last_record_hash does not match the covered record";
            out.ack_window = head_ack ? AckWindowStatus::Unknown : AckWindowStatus::None;
            return out;
        }
        if (anchor->unterminated_final_record) {
            out.durability_caveat = true;  // coverage proven by an unterminated anchor record
        }
    }

    // 6. No coverage (anchor missing or empty).
    if (!anchor_present) {
        out.anchor_absent = true;
        const bool torn_tail = j.tail == JournalTailKind::Torn;
        const bool has_intents = count_intent_evidence(j) > 0;
        if (has_intents || torn_tail) {
            out.state = ForwardChainState::Blocked;
            if (has_intents || !j.tail_type_hint.has_value() ||
                *j.tail_type_hint == JournalRecordType::Intent) {
                out.reason = ForwardBlockReason::AnchorMissingWithIntents;
                out.detail = "INTENT evidence exists without any anchor record; a "
                             "previously authorized rename cannot be excluded";
            } else {
                out.reason = ForwardBlockReason::ProtocolViolation;
                out.detail = "torn tail of an impossible shape without any anchor record";
            }
            out.ack_window = ack_status(head_ack, 0, false);
            return out;
        }
        // RUN-START only, clean end: declared-inert with anchor-absent
        // disclosure (docs section 4.1). Renames are gated on anchored
        // INTENTs; the journal contains none.
        out.state = ForwardChainState::DeclaredInert;
        out.detail = "RUN-START only, zero INTENT records; no rename was ever "
                     "attempted (bounded by R25)";
        out.ack_window = ack_status(head_ack, 0, true);  // dormant ack preserved
        return out;
    }

    // 7. Coverage exists: uncover the journal tail beyond the proven prefix.
    const std::size_t covered_count = cov->covered_seq + 1;
    const std::size_t uncovered_complete = j.records.size() - covered_count;
    const bool torn_tail = j.tail == JournalTailKind::Torn;
    const std::size_t uncovered_things = uncovered_complete + (torn_tail ? 1 : 0);
    const std::size_t covered_intents = count_type(j.records, covered_count,
                                                   JournalRecordType::Intent);
    const std::size_t planned = rs->planned_move_count;

    // Torn/failed anchor tail: harmless only when uncovered(J) is empty
    // (docs section 4.1, W6, T-70b).
    if (anchor->tail == JournalTailKind::Torn) {
        if (uncovered_things > 0) {
            out.state = ForwardChainState::Blocked;
            out.reason = ForwardBlockReason::TornAnchorTailWithContent;
            out.detail = "torn anchor tail while the journal has content beyond the "
                         "proven prefix; an INTENT whose anchoring is now unprovable "
                         "cannot be excluded";
            out.ack_window = ack_status(head_ack, covered_intents, false);
            return out;
        }
        out.torn_anchor_debris = true;
    }

    // Completeness of the resolved prefix [0, end): the caller passes the
    // inclusive record range; END is never inside a covered prefix (rejected
    // above) and appears only as the single uncovered terminal record.
    const auto evaluate_prefix = [&](std::size_t end) {
        const std::size_t results = count_type(j.records, end, JournalRecordType::Result);
        const std::size_t intents = count_type(j.records, end, JournalRecordType::Intent);
        const JournalRecordType last_type = type_of(j.records[end - 1]);
        out.completed_moves = results;
        if (last_type == JournalRecordType::Intent) {
            // Dangling covered INTENT: the rename was authorized and its
            // outcome is unknown (docs section 6.3 state C).
            out.state = ForwardChainState::Blocked;
            out.reason = ForwardBlockReason::DanglingCoveredIntent;
            out.detail = "a covered INTENT has no RESULT; the rename may have "
                         "happened and its outcome is unknown";
            out.ack_window = ack_status(head_ack, intents, false);
            return;
        }
        if (intents == 0 && results == 0) {
            out.state = ForwardChainState::DeclaredInert;
            out.detail = "RUN-START covered, zero INTENT records; no rename was ever "
                         "attempted (bounded by R25)";
            out.ack_window = ack_status(head_ack, 0, true);
            return;
        }
        if (results >= planned) {
            out.state = ForwardChainState::CompleteUnterminated;  // state A
            out.detail = "every planned move has a final RESULT; RUN-END absent";
        } else {
            out.state = ForwardChainState::PartialUnambiguous;  // state B
            out.completed_moves = results;
            out.detail = "interrupted after " + std::to_string(results) + " of " +
                         std::to_string(planned) + " planned moves; " +
                         std::to_string(planned - results) + " never attempted";
        }
        out.ack_window = ack_status(head_ack, intents, false);
    };

    if (uncovered_things == 0) {
        evaluate_prefix(j.records.size());
        return out;
    }
    if (uncovered_things > 1) {
        // At most one uncovered record is protocol-reachable (R19, W8).
        out.state = ForwardChainState::Blocked;
        out.reason = ForwardBlockReason::MultipleUncoveredRecords;
        out.detail = "more than one uncovered journal record; the protocol stops at "
                     "the first failure";
        out.ack_window = ack_status(head_ack, covered_intents, false);
        return out;
    }

    // Exactly one uncovered thing: a torn tail or one complete record.
    if (torn_tail) {
        if (!j.tail_type_hint.has_value()) {
            out.state = ForwardChainState::Blocked;
            out.reason = ForwardBlockReason::UncoveredTailUntyped;
            out.detail = "torn tail is too short to reveal its record type";
            out.ack_window = ack_status(head_ack, covered_intents, false);
            return out;
        }
        switch (*j.tail_type_hint) {
            case JournalRecordType::Intent:
                // Torn INTENT beyond coverage: blocked, never inert
                // (docs section 4.1, R20, T-60).
                out.state = ForwardChainState::Blocked;
                out.reason = ForwardBlockReason::TornIntentBeyondCoverage;
                out.detail = "a previously complete flushed INTENT may have been "
                             "truncated; a previously authorized rename cannot be "
                             "excluded";
                out.ack_window = ack_status(head_ack, covered_intents, false);
                return out;
            case JournalRecordType::Result:
                out.state = ForwardChainState::Blocked;
                out.reason = ForwardBlockReason::TornResultBeyondCoverage;
                out.detail = "a RESULT is written only after the rename; the outcome "
                             "is unknown";
                out.ack_window = ack_status(head_ack, covered_intents, false);
                return out;
            case JournalRecordType::RunEnd: {
                // Torn terminal RUN-END: terminal rules, disclosed (T-63).
                if (count_type(j.records, j.records.size(), JournalRecordType::Result) !=
                    planned) {
                    out.state = ForwardChainState::Blocked;
                    out.reason = ForwardBlockReason::ProtocolViolation;
                    out.detail = "torn RUN-END while planned RESULTs are missing";
                    out.ack_window = ack_status(head_ack, covered_intents, false);
                    return out;
                }
                out.torn_terminal_end = true;
                out.durability_caveat = true;
                evaluate_prefix(j.records.size());
                return out;
            }
            case JournalRecordType::RunStart:
                out.state = ForwardChainState::Blocked;
                out.reason = ForwardBlockReason::ProtocolViolation;
                out.detail = "duplicate RUN-START";
                out.ack_window = ack_status(head_ack, covered_intents, false);
                return out;
            default:
                out.state = ForwardChainState::Blocked;
                out.reason = ForwardBlockReason::ProtocolViolation;
                out.detail = "undo records in a forward journal";
                out.ack_window = ack_status(head_ack, covered_intents, false);
                return out;
        }
    }

    // One complete uncovered record (uncovered_complete == 1, no torn tail).
    const JournalRecord& rec = j.records.back();
    switch (type_of(rec)) {
        case JournalRecordType::Intent: {
            // Complete INTENT beyond coverage: ambiguous between "anchor
            // never written (rename never attempted)" and "anchor written
            // then lost (rename attempted)" - blocked (docs section 4.1).
            out.state = ForwardChainState::Blocked;
            out.reason = ForwardBlockReason::CompleteIntentBeyondCoverage;
            out.detail = "complete INTENT beyond the proven prefix; the rename gate "
                         "was reached or was not - indistinguishable";
            out.ack_window = ack_status(head_ack, covered_intents, false);
            return out;
        }
        case JournalRecordType::Result: {
            // Complete RESULT following a covered INTENT: the outcome applies
            // with a durability caveat (W3/W4).
            const JournalRecord& prev = j.records[cov->covered_seq];
            if (type_of(prev) != JournalRecordType::Intent) {
                out.state = ForwardChainState::Blocked;
                out.reason = ForwardBlockReason::ProtocolViolation;
                out.detail = "uncovered RESULT does not follow a covered INTENT";
                out.ack_window = ack_status(head_ack, covered_intents, false);
                return out;
            }
            out.uncovered_result_outcome = std::get<JournalResult>(rec).outcome;
            out.durability_caveat = true;
            evaluate_prefix(j.records.size());
            return out;
        }
        case JournalRecordType::RunEnd: {
            if (count_type(j.records, j.records.size(), JournalRecordType::Result) != planned) {
                out.state = ForwardChainState::Blocked;
                out.reason = ForwardBlockReason::ProtocolViolation;
                out.detail = "RUN-END present while planned RESULTs are missing";
                out.ack_window = ack_status(head_ack, covered_intents, false);
                return out;
            }
            out.state = ForwardChainState::Closed;
            out.completed_moves = planned;
            out.durability_caveat = true;  // unanchored terminal END (docs section 7)
            out.detail = "complete unanchored RUN-END; closed with durability caveat";
            out.ack_window = ack_status(head_ack, covered_intents, planned == 0);
            return out;
        }
        default:
            out.state = ForwardChainState::Blocked;
            out.reason = ForwardBlockReason::ProtocolViolation;
            out.detail = "impossible uncovered record shape";
            out.ack_window = ack_status(head_ack, covered_intents, false);
            return out;
    }
}

SegmentClassification classify_segment(const ParsedJournalFile& seg) {
    SegmentClassification out;
    if (!seg.valid) {
        out.state = SegmentState::Blocked;
        out.reason = SegmentBlockReason::ParseInvalid;
        out.detail = seg.failure.detail.empty() ? "segment parse failed" : seg.failure.detail;
        return out;
    }
    if (seg.records.empty()) {
        // Zero complete records (docs section 6.4): the same ratified
        // fail-closed rule as the forward classifier - a torn head that
        // cannot reveal its record type is blocked, never an abandoned
        // attempt initialization; only a head uniquely identifying
        // UNDO-START (or a clean empty file) is.
        if (seg.tail == JournalTailKind::Torn) {
            if (!seg.tail_type_hint.has_value()) {
                out.state = SegmentState::Blocked;
                out.reason = SegmentBlockReason::UncoveredTailUntyped;
                out.detail = "torn tail is too short to reveal its record type";
                return out;
            }
            if (*seg.tail_type_hint == JournalRecordType::UndoStart) {
                out.state = SegmentState::AbandonedAttemptInit;
                out.detail = "attempt initialization abandoned with a torn head record";
                return out;
            }
            out.state = SegmentState::Blocked;
            out.reason = SegmentBlockReason::TransitionViolation;
            out.detail = "torn tail of an impossible shape in an attempt segment";
            return out;
        }
        out.state = SegmentState::AbandonedAttemptInit;
        out.detail = "attempt initialization abandoned with zero records";
        return out;
    }
    const JournalUndoStart* us = head_undo_start(seg);
    if (us == nullptr) {
        out.state = SegmentState::Blocked;
        out.reason = SegmentBlockReason::ParseInvalid;
        out.detail = "not an undo-attempt segment";
        return out;
    }
    const std::size_t candidates = us->attempt_candidate_count;
    const std::size_t results =
        count_type(seg.records, seg.records.size(), JournalRecordType::UndoResult);
    const bool torn_tail = seg.tail == JournalTailKind::Torn;

    if (torn_tail) {
        if (!seg.tail_type_hint.has_value()) {
            out.state = SegmentState::Blocked;
            out.reason = SegmentBlockReason::UncoveredTailUntyped;
            out.detail = "torn tail is too short to reveal its record type";
            return out;
        }
        switch (*seg.tail_type_hint) {
            case JournalRecordType::UndoIntent:
                // Reopened torn UNDO-INTENT: blocked, never inert (T-62) - a
                // complete flushed UNDO-INTENT permits the reverse rename.
                out.state = SegmentState::Blocked;
                out.reason = SegmentBlockReason::TornUndoIntent;
                out.detail = "a previously complete flushed UNDO-INTENT may have been "
                             "truncated; a reverse rename cannot be excluded";
                return out;
            case JournalRecordType::UndoResult:
                out.state = SegmentState::Blocked;
                out.reason = SegmentBlockReason::TornUndoResult;
                out.detail = "a UNDO-RESULT is written only after the reverse rename; "
                             "the outcome is unknown";
                return out;
            case JournalRecordType::UndoEnd: {
                // Torn terminal UNDO-END with all candidates final:
                // complete-unterminated, the fold decides (docs 6.4, R23).
                if (results != candidates) {
                    out.state = SegmentState::Blocked;
                    out.reason = SegmentBlockReason::TornTerminalEndInconsistent;
                    out.detail = "torn UNDO-END while candidate UNDO-RESULTs are missing";
                    return out;
                }
                out.state = SegmentState::CompleteUnterminated;
                out.completed_candidates = candidates;
                out.torn_terminal_end = true;
                out.durability_caveat = true;
                return out;
            }
            default:
                out.state = SegmentState::Blocked;
                out.reason = SegmentBlockReason::TransitionViolation;
                out.detail = "torn tail of an impossible shape in an attempt segment";
                return out;
        }
    }

    // Clean end.
    switch (type_of(seg.records.back())) {
        case JournalRecordType::UndoIntent:
            // Complete flushed UNDO-INTENT without RESULT: the reverse rename
            // was permitted and may have been attempted (T-59).
            out.state = SegmentState::Blocked;
            out.reason = SegmentBlockReason::DanglingUndoIntent;
            out.detail = "a complete UNDO-INTENT has no UNDO-RESULT; the reverse "
                         "rename may have happened and its outcome is unknown";
            return out;
        case JournalRecordType::UndoResult: {
            out.completed_candidates = results;
            out.durability_caveat = true;  // segments are never anchored [G9]
            if (results == candidates) {
                out.state = SegmentState::CompleteUnterminated;
                out.detail = "all candidates final, UNDO-END absent; the fold decides";
            } else {
                out.state = SegmentState::PartialUnambiguous;
                out.detail = "interrupted after " + std::to_string(results) + " of " +
                             std::to_string(candidates) + " candidates; the rest were "
                             "never attempted (write-before-rename)";
            }
            return out;
        }
        case JournalRecordType::UndoStart: {
            // UNDO-START complete, no candidate records: nothing was
            // attempted; the same write-before-rename proof as state B
            // applies with k = 0 (ratification pending - see report).
            out.state = SegmentState::PartialUnambiguous;
            out.completed_candidates = 0;
            out.durability_caveat = true;
            out.detail = "interrupted before the first candidate; nothing was "
                         "attempted (write-before-rename)";
            return out;
        }
        case JournalRecordType::UndoEnd: {
            out.state = SegmentState::Closed;
            out.completed_candidates = candidates;
            out.durability_caveat = true;
            out.detail = "complete UNDO-END; closed with durability caveat";
            return out;
        }
        default:
            out.state = SegmentState::Blocked;
            out.reason = SegmentBlockReason::TransitionViolation;
            out.detail = "impossible record shape in an attempt segment";
            return out;
    }
}

BindingCheck verify_segment_digest(const ParsedJournalFile& segment,
                                   std::span<const unsigned char> forward_journal_bytes) {
    BindingCheck out;
    const JournalUndoStart* us = head_undo_start(segment);
    if (us == nullptr) {
        out.error = "segment did not parse as an undo-attempt segment";
        return out;
    }
    const platform::Sha256Result h = platform::sha256(forward_journal_bytes);
    if (!h.ok) {
        out.error = "SHA-256 provider failed: " + h.error;
        return out;
    }
    if (journal_hex(h.digest) != us->fwd_journal_digest) {
        out.error = "fwd_journal_digest does not match the forward journal bytes";
        return out;
    }
    out.ok = true;
    return out;
}

// ---------------------------------------------------------------------------
// Capacity (docs section 12): exact canonical serialization is the runtime
// authority.
// ---------------------------------------------------------------------------

namespace {

std::string repeat_char(char c, std::size_t n) {
    return std::string(n, c);
}

JournalIntent maximal_intent(const JournalRunStart& start, unsigned long long seq,
                             unsigned long long move_id) {
    JournalIntent r;
    r.run = start.run;
    r.seq = seq;
    r.root_run_seq = start.root_run_seq;
    r.move_id = move_id;
    r.cat_id = repeat_char('a', journal_identity_max_bytes);
    r.file_id = repeat_char('a', journal_identity_max_bytes);
    r.file_ctime = 18446744073709551615ULL;
    r.file_size = 18446744073709551615ULL;
    r.file_mtime = 18446744073709551615ULL;
    r.source_name = repeat_char('a', journal_name_max_bytes);
    r.dest_name = repeat_char('a', journal_name_max_bytes);
    r.dest_cat = Category::Documents;
    return r;
}

JournalResult maximal_result(const JournalRunStart& start, unsigned long long seq,
                             unsigned long long move_id) {
    JournalResult r;
    r.run = start.run;
    r.seq = seq;
    r.root_run_seq = start.root_run_seq;
    r.move_id = move_id;
    r.cat_id = repeat_char('a', journal_identity_max_bytes);
    r.file_id = repeat_char('a', journal_identity_max_bytes);
    r.file_ctime = 18446744073709551615ULL;
    r.file_size = 18446744073709551615ULL;
    r.file_mtime = 18446744073709551615ULL;
    r.source_name = repeat_char('a', journal_name_max_bytes);
    r.dest_name = repeat_char('a', journal_name_max_bytes);
    r.dest_cat = Category::Documents;
    r.outcome = JournalOutcome::DestinationChanged;  // longest ratified name
    r.detail = repeat_char('a', journal_detail_max_bytes);
    return r;
}

} // namespace

CapacityEstimate total_journal_bytes(std::span<const JournalRecord> records) {
    CapacityEstimate out;
    static const std::string kDummyPrev(journal_hex_chars, '0');
    std::size_t total = 0;
    for (const JournalRecord& rec : records) {
        const JournalSerializeResult line = serialize_journal_record(rec, kDummyPrev);
        if (!line.ok) {
            out.error = line.error;
            return out;
        }
        total += line.canonical.size();
    }
    out.ok = true;
    out.bytes = total;
    return out;
}

CapacityEstimate forward_reserve_bytes(const JournalRunStart& start,
                                       unsigned long long planned_move_count) {
    CapacityEstimate out;
    if (planned_move_count > journal_move_limit) {
        out.error = "planned_move_count exceeds the 1,500-move operational limit";
        return out;
    }
    std::vector<JournalRecord> records;
    records.reserve(2 * static_cast<std::size_t>(planned_move_count) + 2);
    records.emplace_back(start);
    for (unsigned long long m = 0; m < planned_move_count; ++m) {
        records.emplace_back(maximal_intent(start, 2 * m + 1, m));
        records.emplace_back(maximal_result(start, 2 * m + 2, m));
    }
    JournalRunEnd end_record;
    end_record.run = start.run;
    end_record.seq = 2 * planned_move_count + 1;
    end_record.root_run_seq = start.root_run_seq;
    end_record.planned_count = planned_move_count;
    end_record.moved_count = planned_move_count;
    end_record.conflict_count = planned_move_count;
    end_record.skipped_count = planned_move_count;
    end_record.failed_count = planned_move_count;
    records.emplace_back(std::move(end_record));
    return total_journal_bytes(records);
}

CapacityEstimate segment_reserve_bytes(const JournalUndoStart& start,
                                       unsigned long long candidate_count) {
    CapacityEstimate out;
    if (candidate_count == 0 || candidate_count > journal_move_limit) {
        out.error = "candidate_count must be within 1..1500";
        return out;
    }
    std::vector<JournalRecord> records;
    records.reserve(2 * static_cast<std::size_t>(candidate_count) + 2);
    records.emplace_back(start);
    for (unsigned long long k = 0; k < candidate_count; ++k) {
        JournalUndoIntent intent;
        intent.run = start.run;
        intent.seq = 2 * k + 1;
        intent.root_run_seq = start.root_run_seq;
        intent.fwd_journal_digest = start.fwd_journal_digest;
        intent.undo_attempt_seq = start.undo_attempt_seq;
        intent.attempt_move_seq = k;
        intent.undo_of_move = k;
        intent.cat_id = repeat_char('a', journal_identity_max_bytes);
        intent.file_id = repeat_char('a', journal_identity_max_bytes);
        intent.file_ctime = 18446744073709551615ULL;
        intent.file_size = 18446744073709551615ULL;
        intent.file_mtime = 18446744073709551615ULL;
        intent.source_name = repeat_char('a', journal_name_max_bytes);
        intent.dest_name = repeat_char('a', journal_name_max_bytes);
        intent.dest_cat = Category::Documents;
        // Build the result from the fully populated intent BEFORE the intent
        // is moved into the record vector: reading moved-from strings would
        // understate (or break) the worst-case reserve.
        JournalUndoResult result;
        result.run = intent.run;
        result.seq = 2 * k + 2;
        result.root_run_seq = intent.root_run_seq;
        result.fwd_journal_digest = intent.fwd_journal_digest;
        result.undo_attempt_seq = intent.undo_attempt_seq;
        result.attempt_move_seq = k;
        result.undo_of_move = k;
        result.cat_id = intent.cat_id;
        result.file_id = intent.file_id;
        result.file_ctime = intent.file_ctime;
        result.file_size = intent.file_size;
        result.file_mtime = intent.file_mtime;
        result.source_name = intent.source_name;
        result.dest_name = intent.dest_name;
        result.dest_cat = intent.dest_cat;
        result.outcome = JournalOutcome::DestinationChanged;  // longest ratified name
        result.detail = repeat_char('a', journal_detail_max_bytes);
        records.emplace_back(std::move(intent));
        records.emplace_back(std::move(result));
    }
    JournalUndoEnd end_record;
    end_record.run = start.run;
    end_record.seq = 2 * candidate_count + 1;
    end_record.root_run_seq = start.root_run_seq;
    end_record.fwd_journal_digest = start.fwd_journal_digest;
    end_record.undo_attempt_seq = start.undo_attempt_seq;
    end_record.planned_count = candidate_count;
    end_record.moved_count = candidate_count;
    end_record.conflict_count = candidate_count;
    end_record.skipped_count = candidate_count;
    end_record.failed_count = candidate_count;
    records.emplace_back(std::move(end_record));
    return total_journal_bytes(records);
}

CapacityEstimate anchor_reserve_bytes(unsigned long long planned_move_count) {
    CapacityEstimate out;
    if (planned_move_count > journal_move_limit) {
        out.error = "planned_move_count exceeds the 1,500-move operational limit";
        return out;
    }
    // 2N+1 anchor records (RUN-END is never anchored). byte_len is bounded by
    // the largest legal journal file; root_run_seq keeps its full 20-digit
    // ceiling (conservative).
    const AnchorRecord maximal{
        18446744073709551615ULL,
        repeat_char('a', journal_identity_max_bytes),
        2 * planned_move_count,
        2 * planned_move_count,
        journal_max_file_bytes,
        std::string(journal_hex_chars, 'a'),
    };
    const std::size_t count = 2 * static_cast<std::size_t>(planned_move_count) + 1;
    const JournalSerializeResult line = serialize_anchor_record(maximal);
    if (!line.ok) {
        out.error = line.error;
        return out;
    }
    out.ok = true;
    out.bytes = count * line.canonical.size();
    return out;
}

} // namespace folderpilot
