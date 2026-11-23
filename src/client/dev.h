#pragma once

// A release build of takclient honours NO environment variables and only the documented
// command-line options (--data, --play-map with --game-mod, --version, --help): the game
// is configured through the menu and the Options screen, never through hidden switches.
// Every TAK_* dev/test/diagnostic hook in the viewer reads its environment through
// devEnv() (util/devenv.h), which compiles to a constant nullptr in a release build
// (NDEBUG) -- so those hooks simply vanish from the shipped binary while staying
// available in debug builds. Use this instead of std::getenv.

#include "util/devenv.h"
