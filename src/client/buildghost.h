#pragma once
// Which of a builder's build orders draws a queued-build ghost.
//
// Builds are ordinary orders, and the one being worked stays at the FRONT of the
// queue until the job ends (popBuildOrder in sim.cpp retires it). Its site is then a
// real unit -- a placed ghost before the builder reaches it, a conjuring body after --
// and it draws itself in the painter list, in its proper ground/air place. The
// queued-build ghosts are drawn after every body, so drawing one for that order as
// well laid a second, washed-out copy of the site over everything around it: a Zhon
// Monarch hovering over the lodestone she was conjuring appeared to fly UNDER it.
//
// Templated so it reads a sim::Unit (tests) and a render UnitR alike: both carry
// `orders` (buildType, manaBuildArea) and buildSiteId, and the site's
// underConstruction and type.

#include <cstddef>

namespace tak::render {

// Does order `index` of `builder` draw a queued-build ghost? `site` is the unit
// builder.buildSiteId names (nullptr when there is none or it is gone).
template <class Unit>
bool buildGhostDrawn(const Unit& builder, size_t index, const Unit* site) {
    if (index >= builder.orders.size()) return false;
    const auto& o = builder.orders[index];
    if (!o.buildType || o.manaBuildArea) return false;
    // The active job: its site already stands in the world and draws itself.
    const bool sited = index == 0 && builder.buildSiteId != 0 && site &&
                       site->underConstruction && site->type == o.buildType;
    return !sited;
}

}  // namespace tak::render
