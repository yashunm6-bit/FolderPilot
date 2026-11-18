// FolderPilot - Phase 7.1c storage adapter unit tests: bootstrap, root-key
// derivation, handle-anchored component safety, the leaf-open protocol, the
// mutation lock, append/flush persistence and capacity admission
// (docs/PHASE7-DESIGN.md sections 3 and 18, T-18 through T-32 analogues).
//
// All fixtures are synthetic directories under the workspace sandbox
// (never %TEMP%, never user data). Smart App Control forbids launching the
// locally linked binary, so these cases are compiled locally and executed on
// hosted CI - compilation success here is not runtime success.

#include <doctest.h>

#include "folderpilot/storage_adapter.hpp"
#include "folderpilot/journal.hpp"
#include "../phase3_fixture.hpp"
#include "native.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <aclapi.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using namespace folderpilot;

// The organized-root analogue: a real sandbox directory opened through the
// ratified path bootstrap, so the storage adapter sees a genuine
// volume-GUID + file-identity payload.
platform::RootOpenResult open_sandbox_root(const std::string& utf8_path) {
    return platform::open_root(utf8_path);
}

struct Bootstrapped {
    fixture::SandboxDir base;
    fixture::SandboxDir organized;
    platform::RootOpenResult root;
    StorageRoot storage;

    explicit Bootstrapped(const std::string& name)
        : base(name + "-base"), organized(name + "-root") {}

    std::filesystem::path leaf_path(const std::string& name) const {
        return std::filesystem::path(base.u8()) / "FolderPilot" / "journals" /
               storage.root_key / name;
    }
    std::filesystem::path journals_path() const {
        return std::filesystem::path(base.u8()) / "FolderPilot" / "journals";
    }
};

void bootstrap_fresh(Bootstrapped& b) {
    b.root = open_sandbox_root(b.organized.u8());
    REQUIRE(b.root.ok);
    b.storage = open_storage_root(b.base.u8(), b.root.root);
    // Failure diagnostics: the classified failure, native code and error
    // text accompany any bootstrap REQUIRE failure.
    INFO("storage failure: " << static_cast<int>(b.storage.failure)
                             << " native_code=" << b.storage.native_code
                             << " error=" << b.storage.error);
    REQUIRE(b.storage.ok);
}

// Reads a leaf's bytes back through the adapter's own ratified read open.
std::string leaf_bytes(const StorageRoot& root, const std::string& name) {
    auto leaf = open_journal_leaf_read(root, name);
    if (!leaf.ok || !leaf.handle.valid()) {
        return {};
    }
    std::string contents;
    char buffer[4096];
    DWORD read = 0;
    while (::ReadFile(leaf.handle.get(), buffer, sizeof(buffer), &read, nullptr) &&
           read > 0) {
        contents.append(buffer, static_cast<std::size_t>(read));
    }
    return contents;
}

// RAII restoration of a captured DACL (test fixtures). The constructor
// captures the target's current DACL and its protection/inheritance state;
// the destructor restores BOTH - the DACL bytes and whether the original
// descriptor protected the DACL from inheritance - so the fixture is left
// exactly as found. Restoration runs on every path, including a mid-case
// assertion abort; the destructor never throws, and restoration failures
// are reported on stderr (never silently swallowed).
class ProtectedDaclGuard {
public:
    explicit ProtectedDaclGuard(const std::filesystem::path& target)
        : target_(target) {
        HANDLE handle = ::CreateFileW(target_.c_str(), READ_CONTROL,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                      nullptr, OPEN_EXISTING,
                                      FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            std::fprintf(stderr, "DACL GUARD: capture open failed (error %lu): %ls\n",
                         static_cast<unsigned long>(::GetLastError()),
                         target_.c_str());
            return;
        }
        const BOOL captured =
            ::GetSecurityInfo(handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
                              nullptr, nullptr, &original_dacl_, nullptr,
                              &descriptor_) == ERROR_SUCCESS;
        if (captured && descriptor_ != nullptr) {
            // Preserve the original protection/inheritance state: the
            // restoration re-applies the protected flag only when the
            // original descriptor carried it.
            WORD control = 0;
            DWORD revision = 0;
            if (::GetSecurityDescriptorControl(descriptor_, &control,
                                               &revision) &&
                (control & SE_DACL_PROTECTED) != 0) {
                original_protected_ = true;
            }
            captured_ = true;
        } else {
            std::fprintf(stderr, "DACL GUARD: capture failed (error %lu)\n",
                         static_cast<unsigned long>(::GetLastError()));
        }
        ::CloseHandle(handle);
    }

    ~ProtectedDaclGuard() {
        if (!captured_) {
            if (descriptor_ != nullptr) {
                ::LocalFree(descriptor_);
            }
            return;
        }
        const unsigned long flags =
            DACL_SECURITY_INFORMATION |
            (original_protected_ ? PROTECTED_DACL_SECURITY_INFORMATION : 0UL);
        const BOOL restored = ::SetNamedSecurityInfoW(
            const_cast<LPWSTR>(target_.c_str()), SE_FILE_OBJECT, flags, nullptr,
            nullptr, original_dacl_, nullptr);
        if (restored != ERROR_SUCCESS) {
            std::fprintf(stderr, "DACL GUARD: restore failed (error %lu): %ls\n",
                         static_cast<unsigned long>(::GetLastError()),
                         target_.c_str());
        }
        ::LocalFree(descriptor_);
    }

    ProtectedDaclGuard(const ProtectedDaclGuard&) = delete;
    ProtectedDaclGuard& operator=(const ProtectedDaclGuard&) = delete;

    bool captured() const { return captured_; }

private:
    std::filesystem::path target_;
    bool captured_ = false;
    bool original_protected_ = false;
    PACL original_dacl_ = nullptr;
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
};

// ---------------------------------------------------------------------------
// Cross-process lock-test support (hosted-only).
// ---------------------------------------------------------------------------

// A spawned lock-test child plus its two unique marker paths (a per-run
// unique directory under the base sandbox, so concurrent or repeated runs
// can never observe each other's markers).
struct LockChild {
    fixture::SpawnedProcess process{};
    unsigned long spawn_error = 0;
    std::filesystem::path marker_dir;
    std::filesystem::path ready;
    std::filesystem::path proceed;
};

// Spawns the lock-test child against the fixture's storage. Fails closed on
// any setup error.
bool spawn_lock_child(const Bootstrapped& b, const std::string& unique_token,
                      LockChild& out) {
    out = LockChild{};
    out.marker_dir =
        std::filesystem::path(b.base.u8()) / ("xp-markers-" + unique_token);
    std::error_code ec;
    std::filesystem::create_directories(out.marker_dir, ec);
    if (ec) {
        return false;
    }
    out.ready = out.marker_dir / "ready.lock";
    out.proceed = out.marker_dir / "proceed.lock";
    std::filesystem::remove(out.ready, ec);
    std::filesystem::remove(out.proceed, ec);

    // The parent holds no mutation.lock handles here (fresh bootstrap: the
    // storage root holds directory handles only), so the child's exclusive
    // acquisition is genuinely uncontended at spawn time.
    return fixture::spawn_process(
        FOLDERPILOT_TESTS_EXE_PATH,
        {"--fp-lock-child", b.organized.u8(), b.base.u8(),
         out.ready.string(), out.proceed.string()},
        out.process, out.spawn_error);
}

// RAII child cleanup: on any failure path the child is terminated (and the
// termination confirmed) before the handle is released. A child that
// already exited is only closed.
struct ChildGuard {
    LockChild* child = nullptr;

    ~ChildGuard() {
        if (child == nullptr || child->process.process == nullptr) {
            return;
        }
        if (!fixture::wait_for_process_exit(child->process.process, 0)) {
            fixture::terminate_process(child->process.process, 10000);
        }
        fixture::close_spawned_process(child->process);
    }
};

// Bounded poll for a marker file's existence (never a naked sleep).
bool wait_for_marker(const std::filesystem::path& marker,
                     std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        std::error_code ec;
        if (std::filesystem::exists(marker, ec)) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

// Reads a marker's full content (the child writes its lock_id there).
std::string read_marker(const std::filesystem::path& marker) {
    std::ifstream in(marker, std::ios::binary);
    std::string content{std::istreambuf_iterator<char>(in),
                        std::istreambuf_iterator<char>()};
    return content;
}

// A per-call unique token for marker-directory naming.
std::string unique_token() {
    return std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
}

} // namespace

// ---------------------------------------------------------------------------
// Bootstrap and root identity.
// ---------------------------------------------------------------------------

TEST_CASE("T-18: storage bootstrap creates the handle-anchored chain") {
    Bootstrapped b("st-boot"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    CHECK(!b.storage.root_key.empty());
    CHECK(b.storage.root_key.size() == 64);  // SHA-256 hex, within the 73 B bound
    // The chain exists on disk: base/FolderPilot/journals/<root-key>.
    CHECK(std::filesystem::is_directory(
        std::filesystem::path(b.base.u8()) / "FolderPilot" / "journals" /
        b.storage.root_key));
    // Reopening the same organized root derives the same key and reuses the
    // same per-root directory (the key is re-derived every session).
    const auto again = open_storage_root(b.base.u8(), b.root.root);
    REQUIRE(again.ok);
    CHECK(again.root_key == b.storage.root_key);
}

TEST_CASE("T-18: the hidden base ancestor is accepted; hidden created components refuse") {
    Bootstrapped b("st-hidden-base"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    // The base ancestor carries FILE_ATTRIBUTE_HIDDEN (like the real
    // AppData): the bootstrap still succeeds.
    REQUIRE(fixture::add_attributes(std::filesystem::path(b.base.u8()),
                                    FILE_ATTRIBUTE_HIDDEN));
    const auto again = open_storage_root(b.base.u8(), b.root.root);
    CHECK(again.ok);

    // A FolderPilot-created component carrying hidden attributes is an
    // unexpected state and refuses (fail-closed).
    Bootstrapped b2("st-hidden-comp"); bootstrap_fresh(b2);
    REQUIRE(b2.storage.ok);
    REQUIRE(fixture::add_attributes(b2.journals_path(), FILE_ATTRIBUTE_HIDDEN));
    const auto refused = open_storage_root(b2.base.u8(), b2.root.root);
    CHECK_FALSE(refused.ok);
    CHECK(refused.failure == StorageFailure::HiddenOrSystem);
}

TEST_CASE("T-28: the root key derives from the volume GUID and file identity") {
    // Pure derivation: identical identities produce identical keys; any
    // identity change produces a different key; the representation is
    // tagged, so mixed representations never compare equal.
    const platform::SourceIdentityEx ex_a{0x1234567890ABCDEFULL, {}};
    platform::SourceIdentityEx ex_b = ex_a;
    ex_b.file_id.Identifier[0] ^= 0x01;  // one bit of the file id changes
    const platform::SourceIdentityLegacy legacy{0x12345678, 0x111, 0x222};

    std::string key_a;
    REQUIRE(derive_root_key("\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\",
                            ex_a, key_a));
    std::string key_a_again;
    REQUIRE(derive_root_key("\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\",
                            ex_a, key_a_again));
    std::string key_b;
    REQUIRE(derive_root_key("\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\",
                            ex_b, key_b));
    std::string key_legacy;
    REQUIRE(derive_root_key("\\\\?\\Volume{11111111-1111-1111-1111-111111111111}\\",
                            legacy, key_legacy));
    std::string key_other_volume;
    REQUIRE(derive_root_key("\\\\?\\Volume{22222222-2222-2222-2222-222222222222}\\",
                            ex_a, key_other_volume));

    CHECK(key_a == key_a_again);
    CHECK(key_a != key_b);
    CHECK(key_a != key_legacy);   // representation-tagged: never cross-equal
    CHECK(key_a != key_other_volume);
    CHECK(key_a.size() == 64);

    // Real filesystem identities: two distinct sandbox roots derive two
    // distinct per-root storage directories (per-root isolation).
    Bootstrapped a("st-root-a"); bootstrap_fresh(a);
    REQUIRE(a.storage.ok);
    Bootstrapped b("st-root-b"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    CHECK(a.storage.root_key != b.storage.root_key);
    CHECK(std::filesystem::is_directory(
        std::filesystem::path(a.base.u8()) / "FolderPilot" / "journals" /
        a.storage.root_key));
    CHECK(std::filesystem::is_directory(
        std::filesystem::path(b.base.u8()) / "FolderPilot" / "journals" /
        b.storage.root_key));
}

// ---------------------------------------------------------------------------
// Component and leaf reparse-point safety.
// ---------------------------------------------------------------------------

TEST_CASE("T-21: reparse-point components refuse at every storage level") {
    Bootstrapped b("st-reparse"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);

    // A junction planted at the journals directory (the real directory is
    // moved aside first - pure fixture manipulation, the adapter never
    // deletes anything). The retained storage handles are released first:
    // open directory handles on the journals tree block the rename below
    // (runtime evidence: MoveFileExW fails Permission denied while
    // StorageRoot still holds journals/<root-key> open). The adapter
    // re-bootstraps after the junction is planted.
    b.storage = StorageRoot{};
    const auto target = std::filesystem::path(b.organized.u8()) / "junction-target";
    std::filesystem::create_directories(target);
    std::filesystem::rename(b.journals_path(), target / "journals-real");
    REQUIRE(fixture::create_junction(b.journals_path(), target));
    const auto refused = open_storage_root(b.base.u8(), b.root.root);
    CHECK_FALSE(refused.ok);
    CHECK(refused.failure == StorageFailure::ReparseLeaf);
    // The junction target is untouched.
    CHECK(std::filesystem::is_directory(target / "journals-real"));
}

TEST_CASE("T-21: leaf reparse safety refuses a journal name planted as a junction") {
    Bootstrapped b("st-leaf-reparse"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    const auto target = std::filesystem::path(b.organized.u8()) / "leaf-target";
    std::filesystem::create_directories(target);
    REQUIRE(fixture::create_junction(b.leaf_path("journal-0001.jsonl"), target));
    // FILE_CREATE collides with the existing junction name before any
    // post-open gate can run: the ratified refuse-on-exists disposition
    // precedes reparse inspection, so the classified outcome is
    // NameCollision. The refusal is equally fail-closed; the post-open
    // ReparseLeaf gate is exercised by the read-open case below.
    const auto refused = create_journal_leaf(b.storage, "journal-0001.jsonl");
    CHECK_FALSE(refused.ok);
    CHECK(refused.failure == StorageFailure::NameCollision);
    // The junction target is untouched (no traversal, no fallback).
    CHECK(std::filesystem::is_directory(target));
}

TEST_CASE("T-21: a journal name planted as a junction refuses the ratified read open") {
    Bootstrapped b("st-leaf-reparse-read"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    const auto target = std::filesystem::path(b.organized.u8()) / "leaf-read-target";
    std::filesystem::create_directories(target);
    REQUIRE(fixture::create_junction(b.leaf_path("journal-0001.jsonl"), target));
    // FILE_OPEN + FILE_OPEN_REPARSE_POINT opens the reparse point itself;
    // the post-open leaf gate then refuses: the target is never followed
    // or touched.
    const auto read = open_journal_leaf_read(b.storage, "journal-0001.jsonl");
    CHECK_FALSE(read.ok);
    CHECK(read.failure == StorageFailure::ReparseLeaf);
    CHECK(std::filesystem::is_directory(target));
}

// ---------------------------------------------------------------------------
// Mutation lock.
// ---------------------------------------------------------------------------

TEST_CASE("T-24/T-25: exclusive mutation lock with the ratified probe triple") {
    Bootstrapped b("st-lock"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);

    // Never created.
    auto probe0 = probe_mutation_lock(b.storage);
    REQUIRE(probe0.ok);
    CHECK(probe0.state == LockState::NeverCreated);

    // First acquisition creates the lock file and holds it exclusively
    // (share 0: any second acquirer fails).
    auto lock = acquire_mutation_lock(b.storage);
    REQUIRE(lock.ok);
    REQUIRE(lock.handle.valid());
    CHECK(!lock.lock_id.empty());

    // Held: the probe reports it; a second exclusive acquisition refuses.
    auto probe1 = probe_mutation_lock(b.storage);
    REQUIRE(probe1.ok);
    CHECK(probe1.state == LockState::Held);
    auto second = acquire_mutation_lock(b.storage);
    CHECK_FALSE(second.ok);
    CHECK(second.failure == StorageFailure::SharingViolation);

    // The lock file is never deleted: after release it exists and is free.
    lock.handle.reset();
    auto probe2 = probe_mutation_lock(b.storage);
    REQUIRE(probe2.ok);
    CHECK(probe2.state == LockState::Free);
    CHECK(std::filesystem::exists(b.leaf_path("mutation.lock")));

    // Reacquisition after normal release succeeds with the SAME lock_id.
    // Scope note (lock_id semantics): this asserts the PINNED derivation's
    // stability for one lock-file identity (the lock file is never deleted),
    // the precondition for the ratified cross-session continuity check
    // (design section 3, R11) - not itself a contract-mandated equality;
    // the mandated recorded-vs-current refusal is enforced from 7.2 onward.
    auto reacquired = acquire_mutation_lock(b.storage);
    REQUIRE(reacquired.ok);
    CHECK(reacquired.lock_id == lock.lock_id);
}

TEST_CASE("T-28: lock_id continuity is verified against the recorded value") {
    Bootstrapped b("st-lock-id"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    auto lock = acquire_mutation_lock(b.storage);
    REQUIRE(lock.ok);
    const std::string recorded = lock.lock_id;
    CHECK(verify_mutation_lock_id(lock, recorded).ok);
    CHECK_FALSE(verify_mutation_lock_id(lock, recorded + "x").ok);
    CHECK_FALSE(verify_mutation_lock_id(lock, "").ok);
    const auto mismatch = verify_mutation_lock_id(lock, std::string(64, 'f'));
    CHECK(mismatch.failure == StorageFailure::IdentityMismatch);
}

// ---------------------------------------------------------------------------
// Leaves: creation, refusal, persistence, immutability.
// ---------------------------------------------------------------------------

TEST_CASE("T-22/T-23: fresh leaf creation refuses an existing name") {
    Bootstrapped b("st-create"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    auto first = create_journal_leaf(b.storage, "journal-0001.jsonl");
    // Failure diagnostics: classified failure, native code and error text
    // accompany the REQUIRE below.
    INFO("create failure=" << static_cast<int>(first.failure)
                           << " native_code=" << first.native_code
                           << " error=" << first.error);
    REQUIRE(first.ok);
    CHECK(first.created);
    // FILE_CREATE refuses on exists: the ratified record files are never
    // overwritten, and the existing bytes are untouched.
    auto second = create_journal_leaf(b.storage, "journal-0001.jsonl");
    CHECK_FALSE(second.ok);
    CHECK(second.failure == StorageFailure::NameCollision);
    CHECK(fixture::file_content_matches(b.leaf_path("journal-0001.jsonl"), ""));
}

TEST_CASE("T-26: unsafe leaf names are refused before any filesystem call") {
    Bootstrapped b("st-names"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    for (const std::string bad : {"", ".", "..", "a/b", "a\\b", "a:b", "x/../y"}) {
        const auto refused = create_journal_leaf(b.storage, bad);
        CHECK_FALSE(refused.ok);
        CHECK(refused.failure == StorageFailure::InvalidName);
    }
}

TEST_CASE("T-27: journal writer and preview reader sharing is compatible") {
    Bootstrapped b("st-share"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    auto writer = create_journal_leaf(b.storage, "journal-0001.jsonl");
    REQUIRE(writer.ok);
    REQUIRE(append_journal_leaf_bytes(writer.handle, "record-1\n").ok);
    // The ratified reader open (FILE_GENERIC_READ, share R|W|D) succeeds
    // while the writer holds GENERIC_WRITE share R.
    auto reader = open_journal_leaf_read(b.storage, "journal-0001.jsonl");
    REQUIRE(reader.ok);
    // A second GENERIC_WRITE open fails: the writer shares read only.
    auto second_writer = platform::open_storage_leaf(
        b.storage.root_key_dir.get(), "journal-0001.jsonl",
        GENERIC_WRITE | SYNCHRONIZE, FILE_SHARE_READ, FILE_OPEN);
    CHECK_FALSE(second_writer.ok);
    CHECK(second_writer.failure == platform::NativeFailure::SharingConflict);
}

TEST_CASE("T-30: append order and flush persistence are exact") {
    Bootstrapped b("st-append"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    auto leaf = create_journal_leaf(b.storage, "journal-0001.jsonl");
    REQUIRE(leaf.ok);

    REQUIRE(append_journal_leaf_bytes(leaf.handle, "record-1\n").ok);
    REQUIRE(flush_journal_leaf(leaf.handle).ok);
    CHECK(leaf_bytes(b.storage, "journal-0001.jsonl") == "record-1\n");

    REQUIRE(append_journal_leaf_bytes(leaf.handle, "record-2\n").ok);
    REQUIRE(flush_journal_leaf(leaf.handle).ok);
    // Appends land at the sequential position: order and bytes are exact,
    // and the prefix written before the flush is never rewritten.
    CHECK(leaf_bytes(b.storage, "journal-0001.jsonl") == "record-1\nrecord-2\n");
}

TEST_CASE("T-31: a torn tail from an interrupted append is preserved and classifies") {
    Bootstrapped b("st-torn"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    auto leaf = create_journal_leaf(b.storage, "journal-0001.jsonl");
    REQUIRE(leaf.ok);

    // One genuine canonical record through the journal core, then a torn
    // prefix of the next record: the write is interrupted (simulated by the
    // test as the crash), leaving partial bytes. The adapter never repairs.
    JournalRunStart start;
    start.run = "st-torn-run";
    start.root_run_seq = 1;
    start.planned_move_count = 1;
    start.lock_id = std::string(73, 'i');
    start.root_id = std::string(73, 'i');
    start.anchor_id = std::string(73, 'i');
    start.volume = "\\\\?\\Volume{01234567-8901-2345-6789-0123456789ab}\\";
    start.root_path = "C:\\organized-root";
    const auto chain = serialize_journal_chain(
        std::vector<JournalRecord>{JournalRecord{start}}, "root-key");
    REQUIRE(chain.ok);
    const std::string torn_tail = "{\"v\":1,\"run\":\"0123456789abcdef0123456789ab";
    REQUIRE(append_journal_leaf_bytes(leaf.handle, chain.canonical).ok);
    REQUIRE(append_journal_leaf_bytes(leaf.handle, torn_tail).ok);
    leaf.handle.reset();

    // The interrupted bytes are on disk exactly as the crash would leave
    // them. Reopen and classify: the first record completes, the tail is
    // torn, fail-closed.
    const std::string contents = leaf_bytes(b.storage, "journal-0001.jsonl");
    const std::vector<unsigned char> bytes(contents.begin(), contents.end());
    const auto parsed = parse_journal_file(bytes, "root-key");
    REQUIRE(parsed.valid);
    CHECK(parsed.records.size() == 1);
    CHECK(parsed.tail == JournalTailKind::Torn);
    CHECK_FALSE(parsed.tail_bytes.empty());
}

// ---------------------------------------------------------------------------
// Capacity admission.
// ---------------------------------------------------------------------------

TEST_CASE("T-32: the aggregate quota admits within and refuses past the limit") {
    Bootstrapped b("st-quota"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    auto leaf = create_journal_leaf(b.storage, "journal-0001.jsonl");
    REQUIRE(leaf.ok);
    REQUIRE(append_journal_leaf_bytes(leaf.handle, "journal-bytes\n").ok);
    leaf.handle.reset();

    // "journal-bytes\n" is exactly 14 bytes; the over-quota branch below
    // requests one byte more than the remaining headroom so both assertions
    // genuinely exercise the 128 MiB boundary.
    const unsigned long long used = 14;
    // Within the quota: the remaining headroom accommodates the reserve.
    auto within = check_aggregate_quota(b.storage, 1);
    REQUIRE(within.ok);
    CHECK(within.existing_bytes == used);

    // Past the quota: the exact recompute refuses (128 MiB minus the used
    // bytes is the most that can be reserved).
    const auto refused =
        check_aggregate_quota(b.storage, journal_max_aggregate_bytes - used + 1);
    CHECK_FALSE(refused.ok);
    CHECK(refused.failure == StorageFailure::QuotaExceeded);
}

TEST_CASE("T-33: the ratified 16 MiB per-file limit is enforced on appends") {
    Bootstrapped b("st-filecap"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    auto leaf = create_journal_leaf(b.storage, "journal-0001.jsonl");
    REQUIRE(leaf.ok);
    // Fill the file to exactly the ratified limit (one megabyte at a time).
    const std::string chunk(1024 * 1024, 'x');
    for (int i = 0; i < 16; ++i) {
        REQUIRE(append_journal_leaf_bytes(leaf.handle, chunk).ok);
    }
    // One more byte is refused before any byte is written (no partial
    // append across the ratified limit).
    const auto refused = append_journal_leaf_bytes(leaf.handle, "x");
    CHECK_FALSE(refused.ok);
    CHECK(refused.failure == StorageFailure::QuotaExceeded);
    long long size = 0;
    const auto sized = platform::storage_file_size(leaf.handle.get(), size);
    REQUIRE(sized.ok);
    CHECK(size == static_cast<long long>(journal_max_file_bytes));
}

// ---------------------------------------------------------------------------
// ACL validation [G4].
// ---------------------------------------------------------------------------

TEST_CASE("T-29: a journals directory ACL without write access refuses") {
    Bootstrapped b("st-acl"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    const auto journals_path = b.journals_path().wstring();

    // The guard restores the original DACL and its protection state on
    // every path (RAII), including a mid-case assertion abort.
    ProtectedDaclGuard guard(b.journals_path());
    REQUIRE(guard.captured());

    // Build a minimal DACL granting the current user read-only access.
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    PSID users_sid = nullptr;
    REQUIRE(::AllocateAndInitializeSid(
                &nt_authority, 1, SECURITY_BUILTIN_DOMAIN_RID,
                DOMAIN_ALIAS_RID_USERS, 0, 0, 0, 0, 0, 0,
                &users_sid) == TRUE);
    const auto sid_free = std::unique_ptr<void, void (*)(void*)>(
        users_sid, [](void* p) { ::FreeSid(p); });
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = FILE_GENERIC_READ;
    access.grfAccessMode = GRANT_ACCESS;
    access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
    access.Trustee = {};
    access.Trustee.TrusteeType = TRUSTEE_IS_GROUP;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.ptstrName = reinterpret_cast<LPWSTR>(users_sid);
    PACL new_dacl = nullptr;
    REQUIRE(::SetEntriesInAclW(1, &access, nullptr, &new_dacl) == ERROR_SUCCESS);
    const auto new_dacl_free = std::unique_ptr<void, void (*)(void*)>(
        new_dacl, [](void* p) { ::LocalFree(p); });

    // Apply the read-only DACL to the journals directory (fixture setup).
    // PROTECTED_DACL_SECURITY_INFORMATION is mandatory here: without it the
    // inheritable permissive ACEs from the parent directory re-apply, the
    // effective ACL would still grant read/write, and the fixture would test
    // nothing. Fixture setup failures are asserted with the Win32 code.
    const BOOL applied = ::SetNamedSecurityInfoW(
        const_cast<LPWSTR>(journals_path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, new_dacl, nullptr);
    const DWORD applied_error = ::GetLastError();
    REQUIRE_MESSAGE(applied == ERROR_SUCCESS,
                    "SetNamedSecurityInfoW(apply read-only DACL) failed with "
                        << "Win32 error " << applied_error);

    // The bootstrap refuses. Layering: the journals component open itself
    // requests write-class access (FILE_ADD_FILE | FILE_ADD_SUBDIRECTORY),
    // so the OS may deny the open (AccessDenied) before the [G4] ACL
    // validator runs (AclValidationFailed); both are the ratified fail-closed
    // refusal. No other class is accepted.
    const auto refused = open_storage_root(b.base.u8(), b.root.root);
    CHECK_FALSE(refused.ok);
    CHECK((refused.failure == StorageFailure::AccessDenied ||
           refused.failure == StorageFailure::AclValidationFailed));

    // Restoration is the guard's destructor: RAII on every path (including
    // a mid-case assertion abort), with failures reported on stderr.
}

// ---------------------------------------------------------------------------
// Ratified Decision A: absent and NULL DACLs refuse outright; an empty DACL
// (present, zero ACEs) is denied by AccessCheck itself.
// ---------------------------------------------------------------------------

TEST_CASE("Decision A: absent and NULL DACLs refuse; empty DACL denies through AccessCheck") {
    Bootstrapped b("st-dacl-policy"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    const auto journals_path = b.journals_path().wstring();

    // (a) NULL DACL on the journals directory. The component OPEN succeeds
    // (a NULL DACL grants everyone), so the refusal is exercised at the [G4]
    // ACL validator itself - the AclValidationFailed path.
    {
        ProtectedDaclGuard guard(b.journals_path());
        REQUIRE(guard.captured());
        const BOOL applied = ::SetNamedSecurityInfoW(
            const_cast<LPWSTR>(journals_path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, nullptr, nullptr);
        const DWORD applied_error = ::GetLastError();
        REQUIRE_MESSAGE(applied == ERROR_SUCCESS,
                        "SetNamedSecurityInfoW(NULL DACL) failed with "
                            << "Win32 error " << applied_error);
        const auto refused = open_storage_root(b.base.u8(), b.root.root);
        CHECK_FALSE(refused.ok);
        CHECK(refused.failure == StorageFailure::AclValidationFailed);
    }
    // Restoration sanity: with the original DACL back, the bootstrap
    // succeeds again.
    const auto restored_ok = open_storage_root(b.base.u8(), b.root.root);
    INFO("restored bootstrap failure="
         << static_cast<int>(restored_ok.failure)
         << " native_code=" << restored_ok.native_code
         << " error=" << restored_ok.error);
    CHECK(restored_ok.ok);

    // (b) The absent-DACL variant at the descriptor level. A genuinely
    // absent DACL cannot be written to a real NTFS object through supported
    // APIs (SetNamedSecurityInfoW always sets SE_DACL_PRESENT), so this
    // variant exercises the EXACT classification the validator uses on
    // synthetic security descriptors. DOCUMENTED LIMITATION.
    {
        SECURITY_DESCRIPTOR sd{};
        REQUIRE(::InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION));

        // Absent: no DACL present - the pointer is never inspected.
        REQUIRE(::SetSecurityDescriptorDacl(&sd, FALSE, nullptr, FALSE));
        const auto absent = platform::inspect_dacl_presence(&sd);
        REQUIRE(absent.ok);
        CHECK(absent.presence == platform::DaclPresence::Absent);

        // NULL DACL: present, null pointer - refused.
        REQUIRE(::SetSecurityDescriptorDacl(&sd, TRUE, nullptr, FALSE));
        const auto null_dacl = platform::inspect_dacl_presence(&sd);
        REQUIRE(null_dacl.ok);
        CHECK(null_dacl.presence == platform::DaclPresence::NullDacl);

        // Empty DACL: present, zero ACEs - classified Present; the denial
        // happens through AccessCheck (variant (c)).
        std::vector<unsigned char> acl_buffer(sizeof(ACL) + 64, 0);
        auto* empty_acl = reinterpret_cast<PACL>(acl_buffer.data());
        REQUIRE(::InitializeAcl(empty_acl, static_cast<DWORD>(acl_buffer.size()),
                                ACL_REVISION));
        REQUIRE(::SetSecurityDescriptorDacl(&sd, TRUE, empty_acl, FALSE));
        const auto present = platform::inspect_dacl_presence(&sd);
        REQUIRE(present.ok);
        CHECK(present.presence == platform::DaclPresence::Present);
        CHECK(present.dacl == empty_acl);
    }

    // (c) Empty DACL denial through the ACTUAL AccessCheck path: the
    // journals directory handle is pre-opened with READ_CONTROL while the
    // ACL is still permissive, THEN the empty DACL is applied - the opened
    // handle retains its access, so the validator below evaluates the real
    // AccessCheck against the empty DACL. (An open AFTER applying the empty
    // DACL would fail AccessDenied at the component open and would NOT
    // exercise AccessCheck.)
    {
        ProtectedDaclGuard guard(b.journals_path());
        REQUIRE(guard.captured());
        HANDLE preopened = ::CreateFileW(
            journals_path.c_str(), READ_CONTROL,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        REQUIRE(preopened != INVALID_HANDLE_VALUE);

        std::vector<unsigned char> acl_buffer(sizeof(ACL) + 64, 0);
        auto* empty_acl = reinterpret_cast<PACL>(acl_buffer.data());
        REQUIRE(::InitializeAcl(empty_acl, static_cast<DWORD>(acl_buffer.size()),
                                ACL_REVISION));
        const BOOL applied = ::SetNamedSecurityInfoW(
            const_cast<LPWSTR>(journals_path.c_str()), SE_FILE_OBJECT,
            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
            nullptr, nullptr, empty_acl, nullptr);
        const DWORD applied_error = ::GetLastError();
        REQUIRE_MESSAGE(applied == ERROR_SUCCESS,
                        "SetNamedSecurityInfoW(empty DACL) failed with "
                            << "Win32 error " << applied_error);

        const auto denied = platform::validate_current_user_access(
            preopened, FILE_GENERIC_READ | FILE_GENERIC_WRITE);
        ::CloseHandle(preopened);
        INFO("validator failure=" << static_cast<int>(denied.failure)
                                  << " native_code=" << denied.native_code
                                  << " error=" << denied.error);
        REQUIRE(denied.ok);
        CHECK_FALSE(denied.granted);
    }
}

// ---------------------------------------------------------------------------
// Review-wave additions: boundary coverage flagged by the contract and
// security reviews.
// ---------------------------------------------------------------------------

TEST_CASE("T-18: a reparse-pointed storage base refuses") {
    fixture::SandboxDir base("st-base-junction");
    fixture::SandboxDir organized("st-base-junction-root");
    const auto target = std::filesystem::path(base.u8()) / "junction-real";
    std::filesystem::create_directories(target);
    auto junction = std::filesystem::path(base.u8()) / "base-link";
    REQUIRE(fixture::create_junction(junction, target));
    auto root = open_sandbox_root(organized.u8());
    REQUIRE(root.ok);
    // The base itself (the %LOCALAPPDATA% analogue) is a reparse point: the
    // storage chain refuses to follow it. The organized root is valid, so
    // the refusal fires at the base open itself.
    const auto refused = open_storage_root(junction.string(), root.root);
    CHECK_FALSE(refused.ok);
    CHECK(refused.failure == StorageFailure::ReparseLeaf);
    // The junction target is untouched.
    CHECK(std::filesystem::is_directory(target));
}

TEST_CASE("T-18: the system-attribute gate refuses FolderPilot-created components") {
    Bootstrapped b("st-system-comp"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    REQUIRE(fixture::add_attributes(b.journals_path(), FILE_ATTRIBUTE_SYSTEM));
    const auto refused = open_storage_root(b.base.u8(), b.root.root);
    CHECK_FALSE(refused.ok);
    CHECK(refused.failure == StorageFailure::HiddenOrSystem);
}

TEST_CASE("T-24: a reparse-pointed mutation lock refuses acquisition") {
    Bootstrapped b("st-lock-junction"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    const auto target = std::filesystem::path(b.organized.u8()) / "lock-target";
    std::filesystem::create_directories(target);
    REQUIRE(fixture::create_junction(b.leaf_path("mutation.lock"), target));
    const auto refused = acquire_mutation_lock(b.storage);
    CHECK_FALSE(refused.ok);
    CHECK(refused.failure == StorageFailure::ReparseLeaf);
    // The junction target is untouched and the lock file was not created.
    CHECK(std::filesystem::is_directory(target));
}

TEST_CASE("T-22: a directory planted at a leaf name refuses read and create") {
    Bootstrapped b("st-dir-leaf"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    std::filesystem::create_directory(b.leaf_path("journal-0001.jsonl"));
    // Read open: a file leaf was required, a directory was found.
    const auto read = open_journal_leaf_read(b.storage, "journal-0001.jsonl");
    CHECK_FALSE(read.ok);
    CHECK(read.failure == StorageFailure::NotRegularFile);
    // FILE_CREATE refuses on exists.
    const auto created = create_journal_leaf(b.storage, "journal-0001.jsonl");
    CHECK_FALSE(created.ok);
    CHECK(created.failure == StorageFailure::NameCollision);
}

TEST_CASE("T-27: read open of a missing leaf reports NameNotFound") {
    Bootstrapped b("st-missing"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    const auto missing = open_journal_leaf_read(b.storage, "journal-9999.jsonl");
    CHECK_FALSE(missing.ok);
    CHECK(missing.failure == StorageFailure::NameNotFound);
}

TEST_CASE("T-32: a planted directory under <root-key> fails the quota recompute") {
    Bootstrapped b("st-quota-dir"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);
    std::filesystem::create_directory(b.leaf_path("planted"));
    const auto refused = check_aggregate_quota(b.storage, 1);
    CHECK_FALSE(refused.ok);
    // Fail-closed: the exact aggregate recompute cannot be trusted. The
    // implementation deterministically refuses a planted directory with
    // NotRegularFile.
    CHECK(refused.failure == StorageFailure::NotRegularFile);
}

// ---------------------------------------------------------------------------
// Cross-process mutation lock (hosted-only: these cases spawn this test
// executable as a child process; the in-process cases above cannot establish
// cross-process exclusivity).
// ---------------------------------------------------------------------------

TEST_CASE("T-31 (cross-process): a lock held by another process refuses acquisition; clean exit releases it") {
    Bootstrapped b("st-xproc-hold"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);

    LockChild child;
    REQUIRE(spawn_lock_child(b, unique_token(), child));
    ChildGuard guard{&child};

    // Bounded readiness wait. The marker exists ONLY after the child's
    // acquisition succeeded, and its content is the child's lock_id.
    REQUIRE(wait_for_marker(child.ready, std::chrono::seconds(30)));
    const std::string child_lock_id = read_marker(child.ready);
    CHECK(!child_lock_id.empty());

    // The parent holds no lock handles; the probe observes the CHILD's hold
    // through the kernel's global share accounting - a genuine cross-process
    // observation, unlike the in-process probe test above.
    auto probe = probe_mutation_lock(b.storage);
    REQUIRE(probe.ok);
    CHECK(probe.state == LockState::Held);
    auto second = acquire_mutation_lock(b.storage);
    CHECK_FALSE(second.ok);
    CHECK(second.failure == StorageFailure::SharingViolation);

    // Signal the clean-release path; confirm the child exited with 0.
    {
        std::ofstream proceed(child.proceed, std::ios::binary | std::ios::trunc);
        proceed << "go";
    }
    REQUIRE(fixture::wait_for_process_exit(child.process.process, 30000));
    CHECK(fixture::process_exit_code(child.process.process) == 0);
    fixture::close_spawned_process(child.process);
    guard.child = nullptr; // the handle is already released above

    // The kernel closed the child's handle: the lock is free again, and the
    // parent's acquisition of the SAME lock file yields the SAME digest the
    // child recorded. Scope note (lock_id semantics): this verifies that the
    // adapter's PINNED lock_id derivation is stable for one lock-file
    // identity across processes - the precondition for the ratified
    // cross-session continuity check (design section 3, R11), not itself a
    // contract-mandated equality; the mandated recorded-vs-current refusal
    // is enforced from 7.2 onward.
    auto free_probe = probe_mutation_lock(b.storage);
    REQUIRE(free_probe.ok);
    CHECK(free_probe.state == LockState::Free);
    auto reacquired = acquire_mutation_lock(b.storage);
    REQUIRE(reacquired.ok);
    CHECK(reacquired.lock_id == child_lock_id);
}

TEST_CASE("T-32 (cross-process): OS termination of the lock holder releases the mutation lock") {
    Bootstrapped b("st-xproc-kill"); bootstrap_fresh(b);
    REQUIRE(b.storage.ok);

    LockChild child;
    REQUIRE(spawn_lock_child(b, unique_token(), child));
    ChildGuard guard{&child};

    REQUIRE(wait_for_marker(child.ready, std::chrono::seconds(30)));
    auto probe = probe_mutation_lock(b.storage);
    REQUIRE(probe.ok);
    CHECK(probe.state == LockState::Held);

    // Terminate WITHOUT the proceed signal: the child never releases
    // cleanly. Termination is confirmed through the process handle.
    REQUIRE(fixture::terminate_process(child.process.process, 10000));
    fixture::close_spawned_process(child.process);
    guard.child = nullptr; // the handle is already released above

    // The kernel closed the terminated process's handles: crash-release
    // with no stale state, then a clean immediate acquisition.
    auto free_probe = probe_mutation_lock(b.storage);
    REQUIRE(free_probe.ok);
    CHECK(free_probe.state == LockState::Free);
    auto reacquired = acquire_mutation_lock(b.storage);
    REQUIRE(reacquired.ok);
    CHECK(!reacquired.lock_id.empty());
}
