// FolderPilot - Phase 7.2 undo planning implementation (strictly read-only).
//
// Newest-chain selection, ratified classification, the §9.1 fold over every
// attempt segment, and the §9.3 per-candidate reconciliation against the
// live filesystem. Nothing here mutates: no lock, no journal writes, no
// renames.

#include "folderpilot/undo_plan.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <optional>
#include <utility>

#include "folderpilot/storage_adapter.hpp"
#include "storage.hpp"
#include "native.hpp"

namespace folderpilot {

namespace {

// Recognized-name parsing shared with the journal session (the same
// conventions; see §20.1). Returns nullopt for unrecognized/malformed names.
enum class StorageEntryKind { Lock, Journal, Anchor, Undo };
struct StorageEntryClass {
    StorageEntryKind kind = StorageEntryKind::Lock;
    unsigned long long r = 0;
    unsigned long long a = 0;
};

bool parse20(std::wstring_view digits, unsigned long long& out) {
    if (digits.size() != 20) {
        return false;
    }
    unsigned long long value = 0;
    for (const wchar_t c : digits) {
        if (c < L'0' || c > L'9') {
            return false;
        }
        const auto digit = static_cast<unsigned long long>(c - L'0');
        if (value > (std::numeric_limits<unsigned long long>::max() - digit) / 10) {
            return false;
        }
        value = value * 10ULL + digit;
    }
    out = value;
    return true;
}

std::optional<StorageEntryClass> classify_storage_name(std::wstring_view n) {
    if (n == L"mutation.lock") {
        return StorageEntryClass{StorageEntryKind::Lock, 0, 0};
    }
    if (n.size() == 34 && n.starts_with(L"journal-") && n.ends_with(L".jsonl")) {
        unsigned long long r = 0;
        if (!parse20(n.substr(8, 20), r)) {
            return std::nullopt;
        }
        return StorageEntryClass{StorageEntryKind::Journal, r, 0};
    }
    if (n.size() == 33 && n.starts_with(L"anchor-") && n.ends_with(L".jsonl")) {
        unsigned long long r = 0;
        if (!parse20(n.substr(7, 20), r)) {
            return std::nullopt;
        }
        return StorageEntryClass{StorageEntryKind::Anchor, r, 0};
    }
    if (n.size() == 52 && n.starts_with(L"undo-") && n.ends_with(L".jsonl")) {
        unsigned long long r = 0;
        unsigned long long a = 0;
        if (!parse20(n.substr(5, 20), r) || n[25] != L'-' ||
            !parse20(n.substr(26, 20), a)) {
            return std::nullopt;
        }
        return StorageEntryClass{StorageEntryKind::Undo, r, a};
    }
    return std::nullopt;
}

std::string seq_name(const char* prefix, unsigned long long seq) {
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "%s%020llu.jsonl", prefix,
                  static_cast<unsigned long long>(seq));
    return buf;
}

// Reads a leaf through the ratified read open; a missing leaf is an empty
// result (distinct from a read failure).
bool read_leaf(const StorageRoot& storage, const std::string& name,
               std::string& out, bool& missing) {
    out.clear();
    missing = false;
    auto leaf = open_journal_leaf_read(storage, name);
    if (!leaf.ok) {
        if (leaf.failure == StorageFailure::NameNotFound) {
            missing = true;
            return true;
        }
        return false;
    }
    char buffer[8192];
    DWORD read = 0;
    while (::ReadFile(leaf.handle.get(), buffer, sizeof(buffer), &read, nullptr) &&
           read > 0) {
        out.append(buffer, static_cast<std::size_t>(read));
    }
    return true;
}

} // namespace

UndoPlanResult load_undo_plan(std::string_view storage_base_path,
                              const platform::RootDirectory& organized_root) {
    UndoPlanResult out;

    if (!organized_root.valid()) {
        out.error = "undo planning requires a validated organized root";
        return out;
    }
    auto storage = open_storage_root(storage_base_path, organized_root);
    if (!storage.ok) {
        out.error = "storage root: " + storage.error;
        return out;
    }

    // Newest-chain selection by name enumeration (unrecognized entries are
    // fine for read-only planning - they refuse mutation elsewhere - but a
    // malformed recognized-looking name still blocks classification).
    unsigned long long newest = 0;
    bool saw_journal = false;
    bool unrecognized = false;
    std::vector<unsigned long long> attempt_seqs;   // per forward seq (R)
    const auto enumerated = platform::enumerate_immediate_children(
        storage.root_key_dir.get(), 64 * 1024,
        [&](const platform::RawDirEntry& entry) {
            if (entry.name == L"." || entry.name == L"..") {
                return;
            }
            const auto classified = classify_storage_name(entry.name);
            if (!classified) {
                unrecognized = true;
                return;
            }
            if (classified->kind == StorageEntryKind::Journal) {
                if (!saw_journal || classified->r > newest) {
                    newest = classified->r;
                }
                saw_journal = true;
            } else if (classified->kind == StorageEntryKind::Undo) {
                attempt_seqs.push_back(classified->r);
                // An attempt is only folded when it belongs to the newest
                // forward run; the mapping is applied after selection.
            }
        });
    if (!enumerated.ok) {
        out.error = "storage directory enumeration failed: " + enumerated.error;
        return out;
    }
    if (!saw_journal) {
        out.ok = true;
        out.fold.state = ForwardChainState::AbandonedForwardInit;
        out.fold.detail = unrecognized ? "the storage contains unrecognized entries"
                                       : "no forward run exists";
        return out;
    }
    // NOTE: unrecognized entries leave the read-only planning proceeding on
    // the recognized newest chain; the mutation path refuses them (§20.2).

    out.root_run_seq = newest;
    out.journal_name = seq_name("journal-", newest);
    out.anchor_name = seq_name("anchor-", newest);

    // Read the newest journal and its anchor.
    std::string journal_bytes;
    bool journal_missing = false;
    if (!read_leaf(storage, out.journal_name, journal_bytes, journal_missing)) {
        out.error = "the newest journal could not be read";
        return out;
    }
    if (journal_missing) {
        out.ok = true;
        out.fold.state = ForwardChainState::Blocked;
        out.fold.reason = ForwardBlockReason::JournalBelowCoverage;
        out.fold.detail = "the newest journal is absent (storage inconsistency)";
        return out;
    }
    std::string anchor_bytes;
    bool anchor_missing = false;
    if (!read_leaf(storage, out.anchor_name, anchor_bytes, anchor_missing)) {
        out.error = "the newest anchor could not be read";
        return out;
    }

    const std::vector<unsigned char> journal_image(journal_bytes.begin(),
                                                   journal_bytes.end());
    const auto parsed = parse_journal_file(journal_image, storage.root_key);
    if (!parsed.valid) {
        out.ok = true;
        out.fold.state = ForwardChainState::Blocked;
        out.fold.reason = ForwardBlockReason::ParseInvalid;
        out.fold.detail = "the newest journal failed ratified parsing: " +
                          parsed.failure.detail;
        return out;
    }
    std::optional<ParsedAnchorFile> anchor_parsed;
    if (!anchor_missing && !anchor_bytes.empty()) {
        const std::vector<unsigned char> anchor_image(anchor_bytes.begin(),
                                                      anchor_bytes.end());
        auto parsed_anchor = parse_anchor_file(anchor_image);
        if (!parsed_anchor.valid) {
            out.ok = true;
            out.fold.state = ForwardChainState::Blocked;
            out.fold.reason = ForwardBlockReason::ProtocolViolation;
            out.fold.detail = "the newest anchor failed ratified parsing";
            return out;
        }
        anchor_parsed = std::move(parsed_anchor);
    }

    // Leaf identity digests for the ratified R11 cross-binding (G15 v1).
    auto journal_leaf = open_journal_leaf_read(storage, out.journal_name);
    auto anchor_leaf = open_journal_leaf_read(storage, out.anchor_name);
    ChainIdentityInputs ids;
    if (journal_leaf.ok) {
        ids.expected_journal_id = journal_leaf.id_hex;
    }
    if (anchor_leaf.ok) {
        ids.expected_anchor_id = anchor_leaf.id_hex;
    }

    // Ratified classification of the newest chain against its anchor.
    const auto classification =
        classify_forward_chain(parsed, anchor_parsed ? &*anchor_parsed : nullptr, ids);
    out.fold.state = classification.state;
    out.fold.reason = classification.reason;
    out.fold.ack_window = classification.ack_window;
    out.fold.durability_caveat = classification.durability_caveat;
    out.fold.torn_terminal_end = classification.torn_terminal_end;
    if (classification.state == ForwardChainState::Blocked) {
        out.ok = true;
        out.fold.detail = classification.detail;
        return out;
    }

    // §9.1 fold: candidates = Moved results of the newest run, minus every
    // move an attempt segment recorded as restored. Segments belonging to
    // other forward runs do not fold here.
    std::map<unsigned long long, UndoCandidate> candidates;
    std::optional<unsigned long long> pending_intent;   // last INTENT move_id
    std::map<unsigned long long, JournalIntent> intents;
    for (const JournalRecord& rec : parsed.records) {
        if (const auto* intent = std::get_if<JournalIntent>(&rec)) {
            pending_intent = intent->move_id;
            intents[intent->move_id] = *intent;
            continue;
        }
        if (const auto* result = std::get_if<JournalResult>(&rec)) {
            pending_intent.reset();
            if (result->outcome != JournalOutcome::Moved) {
                continue; // recorded failures are not undo candidates
            }
            UndoCandidate c;
            c.move_id = result->move_id;
            c.source_name = result->source_name;
            c.dest_name = result->dest_name;
            c.dest_cat = result->dest_cat;
            c.file_ctime_filetime = result->file_ctime;
            c.file_id_hex = result->file_id;
            c.file_size = result->file_size;
            c.classification = UndoCandidateClass::Undoable;
            c.detail = "recorded Moved; awaiting reconciliation";
            candidates[result->move_id] = std::move(c);
        }
    }

    // Fold every attempt segment of this forward run (§9.1): a restored
    // UNDO-RESULT removes the candidate; a recorded reverse failure does not.
    // Segments are parsed and classified; a blocked segment keeps the chain
    // forward-blocked per R12 and is disclosed.
    std::sort(attempt_seqs.begin(), attempt_seqs.end());
    for (const unsigned long long a : attempt_seqs) {
        char seg[64] = {};
        std::snprintf(seg, sizeof(seg), "undo-%020llu-%020llu.jsonl", newest, a);
        std::string seg_bytes;
        bool seg_missing = false;
        if (!read_leaf(storage, seg, seg_bytes, seg_missing) || seg_missing) {
            continue; // a missing segment file cannot fold (disclosed state)
        }
        const std::vector<unsigned char> seg_image(seg_bytes.begin(), seg_bytes.end());
        const auto seg_parsed = parse_journal_file(seg_image, storage.root_key);
        if (!seg_parsed.valid) {
            out.fold.has_segments = true;
            out.fold.newest_segment_state = SegmentState::Blocked;
            continue;
        }
        const auto seg_class = classify_segment(seg_parsed);
        out.fold.has_segments = true;
        out.fold.newest_segment_state = seg_class.state;
        for (const JournalRecord& rec : seg_parsed.records) {
            if (const auto* r = std::get_if<JournalUndoResult>(&rec)) {
                if (r->outcome == JournalOutcome::Restored) {
                    candidates.erase(r->undo_of_move);
                }
            }
        }
    }

    // §9.3 reconciliation of each live candidate against the live filesystem
    // (read-only; never an atomic snapshot - the undo executor revalidates
    // immediately before every reverse rename).
    for (auto& kv : candidates) {
        UndoCandidate& c = kv.second;

        // Destination inspection: the category directory (never created
        // here), then the destination file through the opened handle.
        const auto cat = platform::open_category_directory(organized_root.get(),
                                                 category_name(c.dest_cat), false);
        std::optional<platform::SourceOpen> dest;
        bool dest_indeterminate = false;
        if (cat.ok) {
            auto d = platform::open_source(cat.handle.get(), c.dest_name);
            if (d.ok) {
                dest = std::move(d);
            } else if (d.failure != platform::NativeFailure::ConfirmedNotFound) {
                dest_indeterminate = true; // present-but-uninspectable states
            }
        } else if (cat.failure != platform::NativeFailure::ConfirmedNotFound) {
            dest_indeterminate = true;
        }
        const bool dest_present = dest.has_value();

        // Source slot inspection (root-relative single component).
        const auto src = platform::inspect_source(organized_root.get(), c.source_name);
        const bool src_present = src.ok;
        const bool src_indeterminate =
            !src.ok && src.failure != platform::NativeFailure::ConfirmedNotFound;

        // Fingerprint comparison at the destination: identity digest (v1)
        // AND creation time must both match (§13 gating; §20.4).
        bool dest_matches = false;
        if (dest.has_value()) {
            std::string digest;
            dest_matches = storage_leaf_id(dest->identity, digest) &&
                           digest == c.file_id_hex &&
                           dest->creation_time_filetime == c.file_ctime_filetime;
        }
        bool src_matches = false;
        if (src.ok) {
            std::string digest;
            src_matches = storage_leaf_id(src.identity, digest) &&
                          digest == c.file_id_hex &&
                          src.creation_time_filetime == c.file_ctime_filetime;
        }

        if (dest_indeterminate || src_indeterminate) {
            c.classification = UndoCandidateClass::Ambiguous;
            c.detail = "inspection failed; the state cannot be distinguished";
            continue;
        }
        if (dest_present && src_present) {
            c.classification = UndoCandidateClass::Conflict;
            c.detail = "both the destination and the original source slot are occupied";
            continue;
        }
        if (!dest_present && !src_present) {
            c.classification = UndoCandidateClass::Missing;
            c.detail = "both the destination and the original source slot are absent";
            continue;
        }
        if (dest_present && !src_present) {
            if (dest_matches) {
                c.classification = UndoCandidateClass::Undoable;
                c.detail = "matching identity at the destination; the source slot is free";
            } else {
                c.classification = UndoCandidateClass::Replaced;
                c.detail = "the destination identity does not match the recorded fingerprint";
            }
            continue;
        }
        if (src_matches) {
            c.classification = UndoCandidateClass::AlreadyReversed;
            c.detail = "matching identity at the original source slot; absent from the destination";
        } else {
            c.classification = UndoCandidateClass::Replaced;
            c.detail = "the original source slot holds a replacement";
        }
    }

    out.fold.candidates.reserve(candidates.size());
    for (auto& kv : candidates) {
        out.fold.candidates.push_back(std::move(kv.second));
    }
    std::sort(out.fold.candidates.begin(), out.fold.candidates.end(),
              [](const UndoCandidate& a, const UndoCandidate& b) {
                  return a.move_id < b.move_id;
              });
    out.ok = true;
    return out;
}

} // namespace folderpilot
