#include "cartographer/savejob.h"
#include "cartographer/newmap.h"
#include "terrain/terrain.h"
#include <algorithm>
#include <cctype>
#include <stdexcept>
namespace cart {
std::future<SaveResult> saveInBackground(SaveSnapshot snapshot,std::shared_ptr<SaveProgress> progress) {
    return std::async(std::launch::async,[snapshot=std::move(snapshot),progress=std::move(progress)]() mutable {
        SaveResult result;
        try {
            if(progress->cancel.load())throw std::runtime_error("Save cancelled");
            if(snapshot.rebuildMinimaps) {
                progress->phase=SaveProgress::Minimap;
                auto world=snapshot.metadata.kingdom.empty()?std::string("aramon"):snapshot.metadata.kingdom;
                std::transform(world.begin(),world.end(),world.begin(),[](unsigned char c){return char(std::tolower(c));});
                if(!snapshot.vfs)throw std::runtime_error("Save has no terrain asset source");
                tak::terrain::Compositor compositor(*snapshot.vfs);
                auto cancel=std::shared_ptr<const std::atomic_bool>(progress,&progress->cancel);
                generateMinimaps(snapshot.map,compositor,loadWorldPalette(*snapshot.vfs,world),cancel);
            }
            if(progress->cancel.load())throw std::runtime_error("Save cancelled");
            progress->phase=SaveProgress::Serializing;
            const auto text=snapshot.destination.stem().u8string();const std::string name(text.begin(),text.end());
            const auto files=documentFiles(snapshot.map,snapshot.metadata,snapshot.scenario,snapshot.units,snapshot.useOnly,snapshot.aiFiles,name);
            progress->phase=SaveProgress::Writing;
            result.ok=snapshot.bundle?writeDocumentBundle(snapshot.destination,files,result.error,&progress->cancel):
                writeDocumentLoose(snapshot.destination.parent_path(),files,snapshot.installMaps,result.error,&progress->cancel);
        } catch(const std::exception& e) {result.error=e.what();}
        progress->phase=SaveProgress::Finished;
        return result;
    });
}
}
