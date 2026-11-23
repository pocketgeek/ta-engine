#pragma once

// The GAME DATA FOLDER picker run off the UI thread: the native dialog (or a test's
// fake picker) and then hpi::validInstall + rootManifest, which mount the archives and
// take a moment. The window that asked keeps drawing and polls for the outcome.
// Used by Options (client/options.cpp) and Cartographer's Preferences.

#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <optional>
#include <string>

#include "client/datafolder.h"
#include "client/dirpicker.h"
#include "hpi/hpi.h"
#include "util/u8path.h"

namespace tak {

class DataFolderPicker {
public:
    // (title, start folder) -> the chosen folder, "" = cancelled. Empty = the native
    // pickDirectory. Debug/test builds substitute a fake; release builds never do.
    using Picker = std::function<std::string(const std::string&, const std::string&)>;

    // Pick, then judge the pick against `saved` (client/datafolder.h). One at a time.
    void start(Picker picker, std::string start, std::string saved) {
        if (job_.valid()) return;   // running, or finished and not yet collected by poll()
        job_ = std::async(std::launch::async, [picker = std::move(picker), start = std::move(start),
                                               saved = std::move(saved)] {
            const std::string picked = picker ? picker(kDataFolderPickerTitle, start)
                                              : pickDirectory(kDataFolderPickerTitle, start);
            return evaluateDataFolderPick(picked, saved,
                [](const std::string& f, std::string* why) { return hpi::validInstall(tak::u8path(f), why); },
                [](const std::string& f) { return hpi::rootManifest(tak::u8path(f)); });
        });
    }
    // The dialog (or the validation after it) is still running.
    bool busy() const {
        return job_.valid() && job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    }
    // The outcome, once, when it is ready.
    std::optional<DataFolderPick> poll() {
        if (!job_.valid() || job_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return std::nullopt;
        return job_.get();
    }

private:
    std::future<DataFolderPick> job_;   // destroying a running one waits for the dialog to close
};

}  // namespace tak
