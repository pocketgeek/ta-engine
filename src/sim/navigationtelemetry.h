#pragma once
#include <cstddef>
#include <cstdint>

namespace tak::sim {
// Optional service-lifecycle observer for diagnostics. The caller supplies its
// own simulation-tick clock; no wall-clock time, observer result, or token is
// used by navigation, checksums, saves, or scheduling. A duplicate request that
// preserves an in-flight search does not start a second observation.
//
// Keep the observer alive until it is detached or its service is destroyed.
// Attach before submitting requests. Replacing/detaching an observer while
// requests are outstanding is rejected. Tokens are observer-owned and nonzero.
class NavigationTelemetry {
public:
    enum class Cancel {Explicit,Replaced,Stale,Cleared};
    virtual ~NavigationTelemetry()=default;
    virtual uint64_t requested(int unit)=0;
    virtual void cancelled(uint64_t token,Cancel reason)=0;
    // This is invocation of the route-delivery callback, including empty and
    // partial results. It does not mean the unit physically reached its goal.
    virtual void delivered(uint64_t token,bool failed,size_t routePoints)=0;
};
}
