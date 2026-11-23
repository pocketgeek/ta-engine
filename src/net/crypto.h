#pragma once

// SHA-256 / HMAC-SHA-256 / PBKDF2-HMAC-SHA-256, a CSPRNG, and the small helpers
// (constant-time compare, hex, wipe) that the multiplayer login needs.
//
// This is the ONLY cryptography in the engine and it exists for one job: the
// account handshake in `src/net/auth.h`, which is binary SCRAM-SHA-256. Hashing
// is the whole of it -- there is no cipher and no public-key maths here, because
// SCRAM needs none. That is deliberate: hash primitives have official test
// vectors (FIPS 180-4, RFC 4231, RFC 7914), so `authtest` can prove this
// implementation byte-for-byte correct, which is not a claim anyone should make
// about hand-rolled asymmetric crypto.
//
// Nothing here is deterministic-sim code and nothing here is ever hashed into
// World::stateHash(); it runs on the connection, not the simulation.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tak::crypto {

constexpr size_t kHashLen = 32;   // SHA-256 output
constexpr size_t kBlockLen = 64;  // SHA-256 block

using Digest = std::array<uint8_t, kHashLen>;

// ---- SHA-256 ---------------------------------------------------------------

// Incremental SHA-256. Used directly by the HMAC below; callers that have all
// the bytes at once want the one-shot `sha256` overloads instead.
class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    void update(std::string_view s) { update(s.data(), s.size()); }
    Digest final();   // finishes the hash; the object must be reset() to reuse

private:
    void block(const uint8_t* p);
    uint32_t h_[8];
    uint64_t bits_ = 0;
    uint8_t buf_[kBlockLen];
    size_t n_ = 0;
};

Digest sha256(const void* data, size_t len);
inline Digest sha256(std::string_view s) { return sha256(s.data(), s.size()); }

// ---- HMAC-SHA-256 ----------------------------------------------------------

Digest hmacSha256(const void* key, size_t keyLen, const void* msg, size_t msgLen);
inline Digest hmacSha256(std::string_view key, std::string_view msg) {
    return hmacSha256(key.data(), key.size(), msg.data(), msg.size());
}
inline Digest hmacSha256(const Digest& key, std::string_view msg) {
    return hmacSha256(key.data(), key.size(), msg.data(), msg.size());
}
inline Digest hmacSha256(const Digest& key, const std::vector<uint8_t>& msg) {
    return hmacSha256(key.data(), key.size(), msg.data(), msg.size());
}

// HMAC with the key schedule computed once. PBKDF2 runs the same key hundreds of
// thousands of times, and re-absorbing the padded key block on every iteration
// doubles the work for nothing.
class Hmac {
public:
    Hmac(const void* key, size_t keyLen);
    Digest operator()(const void* msg, size_t msgLen) const;

private:
    Sha256 inner_, outer_;   // both primed with the padded key, ready to update()
};

// ---- PBKDF2-HMAC-SHA-256 ---------------------------------------------------

// The deliberately slow one. `iters` is the work factor; see kPbkdf2Iters in
// `auth.h` for the value the login actually uses and why.
std::vector<uint8_t> pbkdf2Sha256(std::string_view password, const void* salt,
                                  size_t saltLen, uint32_t iters, size_t dkLen);

// ---- utilities -------------------------------------------------------------

// Cryptographically secure random bytes from the OS (getrandom / BCryptGenRandom
// / arc4random_buf). Returns false if the OS refuses, which callers MUST treat as
// fatal -- silently falling back to rand() would hand out guessable nonces and
// salts. There is no fallback here on purpose.
[[nodiscard]] bool randomBytes(void* out, size_t len);

// Random bytes as a byte vector; throws std::runtime_error if the OS refuses, so
// a caller that cannot handle failure inline still cannot proceed on bad entropy.
std::vector<uint8_t> randomVec(size_t len);

// Length-independent comparison: no early exit, so the time it takes reveals
// nothing about how many leading bytes matched. Every secret comparison in the
// auth path goes through this.
[[nodiscard]] bool equalCT(const void* a, const void* b, size_t len);
[[nodiscard]] inline bool equalCT(const Digest& a, const Digest& b) {
    return equalCT(a.data(), b.data(), kHashLen);
}

std::string toHex(const void* data, size_t len);
inline std::string toHex(const Digest& d) { return toHex(d.data(), d.size()); }
inline std::string toHex(const std::vector<uint8_t>& v) { return toHex(v.data(), v.size()); }

// Parses exactly `len` bytes of lower- or upper-case hex. Returns false (leaving
// `out` untouched) on a wrong length or a non-hex digit.
[[nodiscard]] bool fromHex(std::string_view hex, void* out, size_t len);
[[nodiscard]] bool fromHex(std::string_view hex, Digest& out);

// Overwrite a buffer that held a secret. Written through a volatile pointer so
// the compiler cannot elide it as a dead store to a soon-dead object -- which is
// exactly what it would do to a plain memset of a local password buffer.
void wipe(void* p, size_t len);
void wipe(std::string& s);

}  // namespace tak::crypto
