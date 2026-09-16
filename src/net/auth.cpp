#include "net/auth.h"

#include <cctype>
#include <cstring>

namespace ta::auth {

namespace {

// Domain separation: these strings are the SCRAM standard's, and they keep the
// two keys derived from one SaltedPassword independent of each other.
constexpr std::string_view kClientKeyLabel = "Client Key";
constexpr std::string_view kServerKeyLabel = "Server Key";

// Version tag opening every authMessage. If the exchange ever changes shape,
// bumping this makes a proof from the old scheme unusable under the new one.
constexpr std::string_view kTranscriptTag = "TA-SCRAM-SHA-256-v1";

void putU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x >> 24)); v.push_back(uint8_t(x >> 16));
    v.push_back(uint8_t(x >> 8));  v.push_back(uint8_t(x));
}

// Length-prefixed append. The prefix is what stops "ab" + "c" from colliding
// with "a" + "bc" and letting two different exchanges share a transcript.
void putField(std::vector<uint8_t>& v, const void* p, size_t n) {
    putU32(v, uint32_t(n));
    const uint8_t* b = static_cast<const uint8_t*>(p);
    v.insert(v.end(), b, b + n);
}
void putField(std::vector<uint8_t>& v, std::string_view s) { putField(v, s.data(), s.size()); }
void putField(std::vector<uint8_t>& v, const std::vector<uint8_t>& b) {
    putField(v, b.data(), b.size());
}

}  // namespace

Keys deriveKeys(std::string_view password, const void* salt, size_t saltLen, uint32_t iters) {
    std::vector<uint8_t> salted = crypto::pbkdf2Sha256(password, salt, saltLen, iters,
                                                       crypto::kHashLen);
    Keys k;
    k.clientKey = crypto::hmacSha256(salted.data(), salted.size(),
                                     kClientKeyLabel.data(), kClientKeyLabel.size());
    k.storedKey = crypto::sha256(k.clientKey.data(), k.clientKey.size());
    k.serverKey = crypto::hmacSha256(salted.data(), salted.size(),
                                     kServerKeyLabel.data(), kServerKeyLabel.size());
    crypto::wipe(salted.data(), salted.size());
    return k;
}

Credential makeCredential(const Keys& k, const void* salt, size_t saltLen, uint32_t iters) {
    Credential c;
    c.iters = iters;
    const uint8_t* s = static_cast<const uint8_t*>(salt);
    c.salt.assign(s, s + saltLen);
    c.storedKey = k.storedKey;
    c.serverKey = k.serverKey;
    return c;
}

std::vector<uint8_t> authMessage(std::string_view user,
                                 const std::vector<uint8_t>& clientNonce,
                                 const std::vector<uint8_t>& serverNonce,
                                 const std::vector<uint8_t>& salt, uint32_t iters) {
    std::vector<uint8_t> m;
    m.reserve(96 + user.size() + clientNonce.size() + serverNonce.size() + salt.size());
    putField(m, kTranscriptTag);
    putField(m, user);
    putField(m, clientNonce);
    putField(m, serverNonce);
    putField(m, salt);
    putU32(m, iters);
    return m;
}

crypto::Digest clientProof(const Keys& k, const std::vector<uint8_t>& am) {
    crypto::Digest sig = crypto::hmacSha256(k.storedKey, am);
    crypto::Digest proof{};
    for (size_t i = 0; i < crypto::kHashLen; ++i) proof[i] = uint8_t(k.clientKey[i] ^ sig[i]);
    return proof;
}

bool verifyClientProof(const Credential& c, const std::vector<uint8_t>& am,
                       const crypto::Digest& proof) {
    // Recover the ClientKey the prover must have held, then check it hashes to
    // what we stored. Note what this never needs: the password, or PBKDF2.
    crypto::Digest sig = crypto::hmacSha256(c.storedKey, am);
    crypto::Digest clientKey{};
    for (size_t i = 0; i < crypto::kHashLen; ++i) clientKey[i] = uint8_t(proof[i] ^ sig[i]);
    crypto::Digest got = crypto::sha256(clientKey.data(), clientKey.size());
    return crypto::equalCT(got, c.storedKey);
}

crypto::Digest serverSignature(const crypto::Digest& serverKey, const std::vector<uint8_t>& am) {
    return crypto::hmacSha256(serverKey, am);
}

// ---- account naming --------------------------------------------------------

bool validUsername(std::string_view u, std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (u.size() < kMinUsername) return fail("that name is too short (3 characters minimum)");
    if (u.size() > kMaxUsername) return fail("that name is too long (20 characters maximum)");
    unsigned char f = (unsigned char)u.front();
    if (!std::isalnum(f)) return fail("a name has to start with a letter or a number");
    for (char ch : u) {
        unsigned char c = (unsigned char)ch;
        if (std::isalnum(c) || c == '_' || c == '-' || c == '.') continue;
        return fail("a name can only use letters, numbers, and _ - .");
    }
    return true;
}

bool validPassword(std::string_view p, std::string* why) {
    auto fail = [&](const char* m) { if (why) *why = m; return false; };
    if (p.size() < kMinPassword) return fail("that password is too short (8 characters minimum)");
    if (p.size() > kMaxPassword) return fail("that password is too long (128 characters maximum)");
    for (char ch : p)
        if ((unsigned char)ch < 0x20 || (unsigned char)ch == 0x7f)
            return fail("that password contains a character that can't be used");
    return true;
}

std::string foldUsername(std::string_view u) {
    std::string s(u);
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

}  // namespace ta::auth
