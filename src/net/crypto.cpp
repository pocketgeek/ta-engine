#include "net/crypto.h"

#include <cstring>
#include <stdexcept>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
  #include <bcrypt.h>
#elif defined(__APPLE__)
  #include <cstdlib>          // arc4random_buf
#else
  #include <sys/random.h>     // getrandom
  #include <cerrno>
  #include <cstdio>
#endif

namespace ta::crypto {

namespace {

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

// FIPS 180-4 round constants: the first 32 bits of the fractional parts of the
// cube roots of the first 64 primes.
const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
inline void putBe32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

}  // namespace

// ---- SHA-256 ---------------------------------------------------------------

void Sha256::reset() {
    // Fractional parts of the square roots of the first 8 primes.
    h_[0] = 0x6a09e667; h_[1] = 0xbb67ae85; h_[2] = 0x3c6ef372; h_[3] = 0xa54ff53a;
    h_[4] = 0x510e527f; h_[5] = 0x9b05688c; h_[6] = 0x1f83d9ab; h_[7] = 0x5be0cd19;
    bits_ = 0;
    n_ = 0;
}

void Sha256::block(const uint8_t* p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = be32(p + i * 4);
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3];
    uint32_t e = h_[4], f = h_[5], g = h_[6], hh = h_[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + maj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h_[0] += a; h_[1] += b; h_[2] += c; h_[3] += d;
    h_[4] += e; h_[5] += f; h_[6] += g; h_[7] += hh;
}

void Sha256::update(const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bits_ += uint64_t(len) * 8;
    if (n_) {                                   // top up a partial buffer first
        size_t take = kBlockLen - n_;
        if (take > len) take = len;
        std::memcpy(buf_ + n_, p, take);
        n_ += take; p += take; len -= take;
        if (n_ == kBlockLen) { block(buf_); n_ = 0; }
    }
    while (len >= kBlockLen) { block(p); p += kBlockLen; len -= kBlockLen; }
    if (len) { std::memcpy(buf_, p, len); n_ = len; }
}

Digest Sha256::final() {
    uint64_t bits = bits_;
    uint8_t pad = 0x80;
    update(&pad, 1);
    bits_ = bits;                               // the padding is not message length
    static const uint8_t zeros[kBlockLen] = {};
    // Pad with zeros until 8 bytes short of a block boundary, leaving room for the
    // 64-bit length. When n_ is already past 56 this rolls into the next block.
    size_t need = (n_ <= 56) ? (56 - n_) : (56 + kBlockLen - n_);
    if (need) { update(zeros, need); bits_ = bits; }
    uint8_t lenBe[8];
    for (int i = 0; i < 8; ++i) lenBe[i] = uint8_t(bits >> (56 - i * 8));
    update(lenBe, 8);
    Digest out{};
    for (int i = 0; i < 8; ++i) putBe32(out.data() + i * 4, h_[i]);
    return out;
}

Digest sha256(const void* data, size_t len) {
    Sha256 s;
    s.update(data, len);
    return s.final();
}

// ---- HMAC-SHA-256 ----------------------------------------------------------

Hmac::Hmac(const void* key, size_t keyLen) {
    uint8_t k[kBlockLen] = {};
    if (keyLen > kBlockLen) {                   // long keys are hashed down first
        Digest d = sha256(key, keyLen);
        std::memcpy(k, d.data(), d.size());
    } else if (keyLen) {
        std::memcpy(k, key, keyLen);
    }
    uint8_t pad[kBlockLen];
    for (size_t i = 0; i < kBlockLen; ++i) pad[i] = uint8_t(k[i] ^ 0x36);
    inner_.update(pad, kBlockLen);
    for (size_t i = 0; i < kBlockLen; ++i) pad[i] = uint8_t(k[i] ^ 0x5c);
    outer_.update(pad, kBlockLen);
    wipe(k, sizeof k);
    wipe(pad, sizeof pad);
}

Digest Hmac::operator()(const void* msg, size_t msgLen) const {
    Sha256 in = inner_;                         // copy the primed state, don't rebuild it
    in.update(msg, msgLen);
    Digest ih = in.final();
    Sha256 out = outer_;
    out.update(ih.data(), ih.size());
    return out.final();
}

Digest hmacSha256(const void* key, size_t keyLen, const void* msg, size_t msgLen) {
    return Hmac(key, keyLen)(msg, msgLen);
}

// ---- PBKDF2-HMAC-SHA-256 ---------------------------------------------------

std::vector<uint8_t> pbkdf2Sha256(std::string_view password, const void* salt,
                                  size_t saltLen, uint32_t iters, size_t dkLen) {
    if (iters == 0) iters = 1;
    std::vector<uint8_t> out(dkLen);
    const Hmac prf(password.data(), password.size());   // key schedule: computed once
    std::vector<uint8_t> first(saltLen + 4);
    if (saltLen) std::memcpy(first.data(), salt, saltLen);

    size_t done = 0;
    for (uint32_t blockIdx = 1; done < dkLen; ++blockIdx) {
        putBe32(first.data() + saltLen, blockIdx);      // U1 = PRF(salt || INT_BE(i))
        Digest u = prf(first.data(), first.size());
        Digest acc = u;
        for (uint32_t i = 1; i < iters; ++i) {
            u = prf(u.data(), u.size());                // U_{n+1} = PRF(U_n)
            for (size_t b = 0; b < kHashLen; ++b) acc[b] ^= u[b];
        }
        size_t take = dkLen - done < kHashLen ? dkLen - done : kHashLen;
        std::memcpy(out.data() + done, acc.data(), take);
        done += take;
    }
    return out;
}

// ---- utilities -------------------------------------------------------------

bool randomBytes(void* out, size_t len) {
    if (len == 0) return true;
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), ULONG(len),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__APPLE__)
    arc4random_buf(out, len);                   // cannot fail by contract
    return true;
#else
    uint8_t* p = static_cast<uint8_t*>(out);
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::getrandom(p + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            // Very old kernels lack the syscall; fall back to the device, which is
            // the same pool. Still no rand() fallback -- if this fails we fail.
            std::FILE* f = std::fopen("/dev/urandom", "rb");
            if (!f) return false;
            size_t rd = std::fread(p + got, 1, len - got, f);
            std::fclose(f);
            return rd == len - got;
        }
        got += size_t(n);
    }
    return true;
#endif
}

std::vector<uint8_t> randomVec(size_t len) {
    std::vector<uint8_t> v(len);
    if (!randomBytes(v.data(), v.size()))
        throw std::runtime_error("the OS random number generator is unavailable");
    return v;
}

bool equalCT(const void* a, const void* b, size_t len) {
    const uint8_t* x = static_cast<const uint8_t*>(a);
    const uint8_t* y = static_cast<const uint8_t*>(b);
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff = uint8_t(diff | (x[i] ^ y[i]));
    return diff == 0;
}

std::string toHex(const void* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::string s;
    s.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        s[i * 2] = kHex[p[i] >> 4];
        s[i * 2 + 1] = kHex[p[i] & 15];
    }
    return s;
}

bool fromHex(std::string_view hex, void* out, size_t len) {
    if (hex.size() != len * 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> tmp(len);
    for (size_t i = 0; i < len; ++i) {
        int hi = nib(hex[i * 2]), lo = nib(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;     // leave `out` untouched on failure
        tmp[i] = uint8_t(hi << 4 | lo);
    }
    std::memcpy(out, tmp.data(), len);
    return true;
}

bool fromHex(std::string_view hex, Digest& out) {
    return fromHex(hex, out.data(), out.size());
}

void wipe(void* p, size_t len) {
    volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
    while (len--) *v++ = 0;
}

void wipe(std::string& s) {
    if (!s.empty()) wipe(&s[0], s.size());
    s.clear();
}

}  // namespace ta::crypto
