// No credential store on this platform: Remember Key stays greyed out. There is
// deliberately no file fallback (see credstore.h).

#include "client/credstore.h"

namespace tak::credstore {
namespace {

class NoStore final : public Store {
public:
    std::string name() const override { return "PASSWORD STORE"; }
    std::string probe() override { return kNoStore; }
    Result load(std::string&, std::string& error) override { error = kNoStore; return Result::Failed; }
    bool store(const std::string&, std::string& error) override { error = kNoStore; return false; }
    bool erase(std::string& error) override { error = kNoStore; return false; }
};

}  // namespace

std::unique_ptr<Store> system(const Slot&) { return std::make_unique<NoStore>(); }

}  // namespace tak::credstore
