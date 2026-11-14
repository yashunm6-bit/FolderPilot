// FolderPilot - SHA-256 via the Windows CNG provider (BCrypt). G1 probe.
//
// Design notes:
//   * every BCrypt call returns NTSTATUS; success is tested with the
//     standard non-negative check (STATUS_SUCCESS == 0),
//   * the hash-object size and digest size are queried from the provider
//     instead of being hard-coded,
//   * the algorithm and hash handles are RAII-guarded and closed on every
//     path (CloseHandle would be wrong for CNG handles, so a small local
//     guard is used rather than platform/unique_handle.hpp),
//   * single-shot hashing only for the G1 probe; incremental hashing can be
//     added later without changing the interface,
//   * no exceptions: every failure is reported through Sha256Result.

#include "sha256.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#include <vector>

namespace folderpilot::platform {

namespace {

// Minimal RAII guard for the void*-based CNG handles; each closes through
// its own BCrypt function (never CloseHandle).
struct alg_closer {
    static void close(void* handle) noexcept { ::BCryptCloseAlgorithmProvider(handle, 0); }
};
struct hash_closer {
    static void close(void* handle) noexcept { ::BCryptDestroyHash(handle); }
};
template <typename Closer>
class bcrypt_guard {
public:
    bcrypt_guard() = default;
    explicit bcrypt_guard(void* handle) noexcept : handle_{handle} {}
    ~bcrypt_guard() {
        if (handle_ != nullptr) {
            Closer::close(handle_);
        }
    }
    bcrypt_guard(const bcrypt_guard&) = delete;
    bcrypt_guard& operator=(const bcrypt_guard&) = delete;
    bool valid() const noexcept { return handle_ != nullptr; }
    void* get() const noexcept { return handle_; }

private:
    void* handle_ = nullptr;
};

Sha256Result fail(std::string message, NTSTATUS status) {
    Sha256Result result;
    result.error = std::move(message);
    result.native_code = static_cast<unsigned long>(status);
    return result;
}

} // namespace

Sha256Result sha256(std::span<const unsigned char> data) {
    Sha256Result result;

    void* alg_raw = nullptr;
    NTSTATUS status = ::BCryptOpenAlgorithmProvider(&alg_raw, BCRYPT_SHA256_ALGORITHM,
                                                   nullptr, 0);
    if (status < 0) {
        return fail("BCryptOpenAlgorithmProvider failed", status);
    }
    bcrypt_guard<alg_closer> alg{alg_raw};

    // Query the hash-object size and the digest size from the provider.
    unsigned long object_length = 0;
    unsigned long digest_length = 0;
    unsigned long written = 0;
    status = ::BCryptGetProperty(alg.get(), BCRYPT_OBJECT_LENGTH,
                                 reinterpret_cast<unsigned char*>(&object_length),
                                 sizeof(object_length), &written, 0);
    if (status < 0) {
        return fail("BCryptGetProperty(BCRYPT_OBJECT_LENGTH) failed", status);
    }
    status = ::BCryptGetProperty(alg.get(), BCRYPT_HASH_LENGTH,
                                 reinterpret_cast<unsigned char*>(&digest_length),
                                 sizeof(digest_length), &written, 0);
    if (status < 0) {
        return fail("BCryptGetProperty(BCRYPT_HASH_LENGTH) failed", status);
    }
    if (digest_length != sha256_digest_bytes) {
        return fail("provider reported an unexpected digest length", status);
    }

    std::vector<unsigned char> hash_object(object_length);
    void* hash_raw = nullptr;
    status = ::BCryptCreateHash(alg.get(), &hash_raw, hash_object.data(),
                                object_length, nullptr, 0, 0);
    if (status < 0) {
        return fail("BCryptCreateHash failed", status);
    }
    bcrypt_guard<hash_closer> hash{hash_raw};

    // Empty input is legal and yields the FIPS 180-4 empty-string digest;
    // skip HashData for it (the freshly created hash state is already the
    // SHA-256 initial state). BCryptHashData takes a non-const pointer and
    // does not modify the input [W32-DOC], hence the const_cast.
    if (!data.empty()) {
        status = ::BCryptHashData(hash.get(),
                                  const_cast<unsigned char*>(data.data()),
                                  static_cast<unsigned long>(data.size()), 0);
        if (status < 0) {
            return fail("BCryptHashData failed", status);
        }
    }

    status = ::BCryptFinishHash(hash.get(), result.digest.data(), digest_length, 0);
    if (status < 0) {
        return fail("BCryptFinishHash failed", status);
    }

    result.ok = true;
    return result;
}

} // namespace folderpilot::platform
