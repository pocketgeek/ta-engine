// credstore_test -- round trip through this platform's REAL credential store (Windows
// Credential Manager, macOS Keychain, Secret Service), under credstore::testSlot() only:
// never the stream key's own slot, and only dummy values. Store -> load -> update -> load
// -> erase -> load empty, and an erase of nothing succeeds.
//
// Exit 77 = skipped (CTest SKIP_RETURN_CODE): no usable store in this session -- a CI
// container without a Secret Service, an ssh session whose keychain is locked, a Windows
// logon session without credentials -- or a store that did not answer within 20 s (an
// unlock prompt nobody is there to answer). Skipping says so on stdout.
//
//   credstore_test          the round trip
//   credstore_test put      leave the first dummy in the test slot (debug screenshots)
//   credstore_test erase    delete the test slot

#include "client/credstore.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace tak::credstore;

static const char* kDummyA = "dummy-credstore-roundtrip-0001";
static const char* kDummyB = "dummy-credstore-roundtrip-0002-longer";

static int skip(const char* why) { std::printf("SKIP: %s\n", why); std::fflush(stdout); return 77; }
static int fail(const char* what) { std::printf("FAIL: %s\n", what); return 1; }

static int run(int argc, char** argv) {
    auto store = tak::credstore::system(testSlot());
    const std::string reason = store->probe();
    std::printf("credstore_test: %s, service \"%s\", account \"%s\"\n", store->name().c_str(),
                testSlot().service.c_str(), testSlot().account.c_str());
    if (!reason.empty()) return skip(reason.c_str());
    std::string err, got;
    auto locked = [&] { return err.find("LOCKED") != std::string::npos; };
    if (argc > 1 && !std::strcmp(argv[1], "put")) return store->store(kDummyA, err) ? 0 : fail(err.c_str());
    if (argc > 1 && !std::strcmp(argv[1], "erase")) return store->erase(err) ? 0 : fail(err.c_str());

    if (!store->erase(err)) return locked() ? skip(err.c_str()) : fail(("clean erase: " + err).c_str());
    if (store->load(got, err) != Result::Missing) return fail("load after erase is not Missing");
    if (!store->store(kDummyA, err)) return locked() ? skip(err.c_str()) : fail(("store: " + err).c_str());
    if (store->load(got, err) != Result::Ok || got != kDummyA) return fail("load does not return the stored dummy");
    wipe(got);
    if (!store->store(kDummyB, err)) return fail(("update: " + err).c_str());
    if (store->load(got, err) != Result::Ok || got != kDummyB) return fail("load does not return the updated dummy");
    wipe(got);
    if (!store->erase(err)) return fail(("erase: " + err).c_str());
    if (store->load(got, err) != Result::Missing || !got.empty()) return fail("load after erase still finds it");
    if (!store->erase(err)) return fail("erasing an empty slot failed");
    std::printf("credstore_test: round trip ok\n");
    return 0;
}

int main(int argc, char** argv) {
    // A store that never answers (an unlock prompt) is a skip, not a hung test run.
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::seconds(20));
        std::printf("SKIP: the credential store did not answer within 20 s (locked or prompting?)\n");
        std::fflush(stdout);
        std::_Exit(77);
    }).detach();
    return run(argc, argv);
}
