#pragma once

namespace tak {

// A mobile builder's queue is intent, not proof that a unit is being conjured.
// Retail starts each production animation against the live output site. Keeping
// that site's identity also lets one-shot builder scripts run again for the next
// item in a continuously non-empty queue.
template<class Transition>
void updateRetailBuilderAnimation(bool& wasBuilding, int& lastWorkId,
        int buildSiteId, int repairId, int reclaimId,
        bool productionQueued, int productionSiteId, bool productionSiteActive,
        bool walking, bool flying, Transition&& transition) {
    const int workId = buildSiteId ? buildSiteId :
                       repairId ? repairId :
                       reclaimId ? reclaimId :
                       productionQueued && productionSiteId && productionSiteActive
                           ? productionSiteId : 0;
    const bool working = workId != 0 && (!walking || (flying && productionSiteActive));
    if (working == wasBuilding && (!working || workId == lastWorkId)) return;

    wasBuilding = working;
    lastWorkId = working ? workId : 0;
    transition(working);
}

enum class RetailFactoryAnimationCall { Activate, Deactivate, StartBuilding, StopBuilding };

// Queue activation owns the yard; output-site transitions own build callbacks.
// When the final output completes, StopBuilding precedes Deactivate.
template<class Transition>
void updateRetailFactoryAnimation(bool& active,int& lastSite,bool underConstruction,
        bool queued,int productionSite,Transition&& transition) {
    const bool nextActive=!underConstruction && queued;
    const int site=underConstruction ? 0 : productionSite;
    if (site!=lastSite && lastSite) transition(RetailFactoryAnimationCall::StopBuilding);
    if (nextActive!=active) {
        active=nextActive;
        transition(active ? RetailFactoryAnimationCall::Activate : RetailFactoryAnimationCall::Deactivate);
    }
    if (site!=lastSite) {
        lastSite=site;
        if (site) transition(RetailFactoryAnimationCall::StartBuilding);
    }
}

} // namespace tak
