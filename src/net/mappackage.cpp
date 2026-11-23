#include "net/mappackage.h"
#include "tnt/tnt.h"
#include "tnt/mapgen.h"
#include "tdf/tdf.h"
#include "tnt/ota.h"
#include "terrain/terrain.h"
#include "terrain/minimap.h"
#include "crt/crt.h"
#include "util/virtualpath.h"
#include "util/storagequota.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <set>
#include <stdexcept>

namespace tak::net::maps {
namespace {
std::string hash(const std::vector<uint8_t>& bytes) {
    return crypto::toHex(crypto::sha256(bytes.data(), bytes.size()));
}
std::string tilePath(uint32_t key) {
    char path[40]; std::snprintf(path, sizeof path, "terrain/%08x.jpg", key); return path;
}
bool safePath(const std::string& p) {
    if (p.empty() || p.size() > 512 || p != hpi::MountSet::key(p) || p.front() == '/' ||
        p.find('\\') != std::string::npos || p.find(':') != std::string::npos) return false;
    size_t start = 0;
    for (size_t i = 0; i <= p.size(); ++i) {
        if (i < p.size() && (uint8_t(p[i]) < 32 || p[i] == 127)) return false;
        if (i == p.size() || p[i] == '/') {
            auto part = p.substr(start, i - start);
            if (part.empty() || part == "." || part == "..") return false;
            start = i + 1;
        }
    }
    return true;
}
// Only companions of the selected map may travel with it. In particular this
// must not permit arbitrary gameplay definitions or executable scripts.
std::set<std::string> companionPaths(const std::string& path) {
    std::set<std::string> result;
    for (const char* extension : {".ota", ".crt", ".tdf", ".txt", ".editor", ".recipe"})
        result.insert(vpath::replaceExtension(path, extension));
    return result;
}
// The map's AI profile files: the Classic AI reads ai/<aiprofile>.txt (the .ota's
// [Map Data] key, any extension dropped), else ai/default.txt, and all of our levels
// the one ai/takengine.txt (ai/builtinprofile.h). The server reads them from the room's
// map package (a game mod's copy wins), so when the map's own archive carries one it
// travels here -- that archive's copy, whichever installed map wins the path in the mount.
std::set<std::string> aiProfilePaths(const hpi::Vfs::Files& files, const std::string& mapPath) {
    // ai::kEngineProfileFile (src/ai is not linked here; ai_profile_test checks the
    // two names agree).
    std::set<std::string> result{"ai/default.txt", "ai/takengine.txt"};
    const auto ota = files.find(vpath::replaceExtension(mapPath, ".ota"));
    if (ota == files.end()) return result;
    std::string key;
    try { key = tnt::Scenario::parse(std::string(ota->second.begin(), ota->second.end())).aiProfile; }
    catch (const std::exception&) { return result; }
    if (const auto dot = key.find_last_of('.');
        dot != std::string::npos && key.find_first_of("/\\", dot) == std::string::npos)
        key.erase(dot);
    if (!key.empty()) {
        const auto path = hpi::MountSet::key("ai/" + key + ".txt");
        if (safePath(path) && path.find('/', 3) == std::string::npos) result.insert(path);
    }
    return result;
}
// Follow feature burn/death chains as well as their sprites and palettes. A
// feature file may define several names, so include dependencies of every
// definition it brings into the room, not only the initially placed feature.
std::set<std::string> featureResources(const tnt::Map& map, const hpi::Vfs& vfs, tak::StopToken stop = {}) {
    std::map<std::string, tdf::Node> definitions;
    size_t featureBytes=0;
    tdf::ParseUsage usage;
    std::map<std::string, std::string> owner;
    for (const auto& path : vfs.list("features")) {
        if (!path.ends_with(".tdf")) continue;
        tdf::Node parsed;
        try {
            const auto data = vfs.read(path);
            featureBytes+=data.size();
            if(featureBytes>(8u<<20) || stop.stop_requested())throw tdf::ParseLimitError("feature parsing budget exceeded/cancelled");
            parsed = tdf::parseText(std::string(data.begin(), data.end()), path, {65536,262144,64u<<20,stop,&usage});
        } catch (const tdf::ParseLimitError&) {throw;}
        catch (const std::exception&) { continue; } // Match the feature loader's handling of unrelated bad defs.
        const auto key = hpi::MountSet::key(path);
        for (const auto& name : parsed.childOrder) owner[hpi::MountSet::key(name)] = key;
        definitions.emplace(key, std::move(parsed));
    }
    std::vector<std::string> pending = map.featureNames;
    std::set<std::string> files;
    auto art = [&](const std::string& path) {
        const auto key = hpi::MountSet::key(path);
        if (safePath(key) && vfs.has(key)) files.insert(key);
    };
    while (!pending.empty()) {
        auto name = hpi::MountSet::key(pending.back()); pending.pop_back();
        auto found = owner.find(name);
        if (found == owner.end() || !files.insert(found->second).second) continue;
        for (const auto& [section, def] : definitions.at(found->second).children) {
            for (const char* chain : {"featureburnt", "featuredead"}) {
                auto next = def.valueOr(chain, ""); if (!next.empty()) pending.push_back(next);
            }
            auto file = def.valueOr("filename", "");
            if (!file.empty()) art("anims/" + file + ".gaf");
            auto world = def.valueOr("world", "aramon");
            art("palettes/" + world + "_features.pcx"); art("palettes/" + world + ".pcx");
            art("palettes/aramon_features.pcx");
            for (const char* flame : {"seqnamefrontflame", "seqnamebackflame"}) {
                auto name = def.valueOr(flame, "");
                if (!name.empty()) for (const char* suffix : {"_4444.taf", "_1555.taf", ".taf", ".gaf"})
                    art("anims/" + name + suffix);
            }
        }
    }
    return files;
}
std::shared_ptr<Package> encode(std::string path, std::shared_ptr<hpi::Vfs::Files> files) {
    Writer w; w.u32(2); w.str(path); w.u32(uint32_t(files->size()));
    for (const auto& [name, data] : *files) {
        if (w.b.size() + data.size() + name.size() + 6 > kMaxBytes)
            throw std::runtime_error("map package exceeds 256 MiB");
        w.str(name); w.u32(uint32_t(data.size())); w.b.insert(w.b.end(), data.begin(), data.end());
    }
    auto p = std::make_shared<Package>(); p->mapPath = std::move(path);
    p->files = std::move(files); p->bytes = std::move(w.b); p->digest = hash(p->bytes); return p;
}
void writeAtomic(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    // Unique staging names also handle a local server sharing its client's data root.
    auto tmp = path; tmp += "." + crypto::toHex(crypto::randomVec(8)) + ".tmp";
    try {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        out.close();
        if (!out) throw std::runtime_error("could not save map file");
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            // Windows cannot rename over an existing file. An identical concurrent
            // writer is fine; a corrupted cache is removed by loadCache instead.
            if (!std::filesystem::exists(path)) throw std::runtime_error("could not publish map file: " + ec.message());
            std::filesystem::remove(tmp);
        }
    } catch (...) { std::error_code ec; std::filesystem::remove(tmp, ec); throw; }
}
}
bool validDigest(const std::string& s) {
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}
static std::shared_ptr<Package> buildUncached(const hpi::Vfs& vfs, const std::string& path) {
    auto files = std::make_shared<hpi::Vfs::Files>();
    (*files)[path] = vfs.read(path);
    const auto map = tnt::Map::load(files->at(path), path);
    for (const auto& companion : companionPaths(path))
        if (auto data = vfs.tryRead(companion)) (*files)[companion] = std::move(*data);
    for (auto key : std::set<uint32_t>(map.tileKeys.begin(), map.tileKeys.end())) {
        auto tile = tilePath(key);
        // Missing art is an error, not a black map silently propagated to peers.
        (*files)[tile] = vfs.read(tile);
    }
    for (const auto& resource : featureResources(map, vfs)) (*files)[resource] = vfs.read(resource);
    // From the map's own archive: another installed map carrying the same file name
    // must not take it (nor a game mod's copy, which the server's lookup prefers anyway).
    for (const auto& ai : aiProfilePaths(*files, path))
        if (auto data = vfs.readFromMapArchive(path, ai); data && !data->empty()) (*files)[ai] = std::move(*data);
    return decode(encode(path, files)->bytes, "");
}

std::shared_ptr<Package> importSnapshot(const hpi::Vfs& base, const std::filesystem::path& path) {
    hpi::Archive archive(path);
    auto files = std::make_shared<hpi::Vfs::Files>();
    std::string mapPath;
    size_t total = 0;
    for (const auto& entry : archive.entries()) if (!entry.isDirectory) {
        if (entry.decompressedSize > kMaxBytes - total) throw std::runtime_error("Test map exceeds the map transfer size limit");
        total += entry.decompressedSize;
        const auto key = hpi::MountSet::key(entry.path);
        if (tak::vpath::extension(key) == ".tnt") {
            if (!mapPath.empty()) throw std::runtime_error("Test Map requires a KMP containing exactly one map");
            mapPath = key;
        }
        if (!files->emplace(key, archive.read(entry)).second)
            throw std::runtime_error("Test map has duplicate file names");
    }
    if (mapPath.empty()) throw std::runtime_error("Test map contains no terrain");
    hpi::Vfs view(&base);
    view.setMapFiles(files);
    const auto validated=buildUncached(view, mapPath); // current snapshot wins over cached revisions
    auto enabled=std::make_shared<hpi::Vfs::Files>(*validated->files);
    auto& ota=(*enabled)[tak::vpath::replaceExtension(mapPath,".ota")];
    const auto metadata=tdf::parseText(std::string(ota.begin(),ota.end()));
    const auto* test=metadata.child("takplaytest");
    if(!test || test->numberOr("authoredscenario",0)==0) {
        // Explicit production playtest opt-in. Ordinary retail maps may carry CRT
        // data too; their existing skirmish setup must remain unchanged. Append a
        // separate section rather than rewriting/dropping unknown retail OTA keys.
        const std::string marker="\n[TAKPlaytest]\n{\nauthoredscenario=1;\n}\n";
        ota.insert(ota.end(),marker.begin(),marker.end());
    }
    return decode(encode(mapPath,std::move(enabled))->bytes, "");
}

bool authoredScenario(const hpi::Vfs& vfs, const std::string& mapPath) {
    try {
        const auto ota=vfs.tryRead(vpath::replaceExtension(mapPath,".ota"));
        if (!ota) return false;
        const std::string text(ota->begin(),ota->end());
        const auto root=tdf::parseText(text);
        const auto* marker=root.child("takplaytest");
        if (!marker || marker->numberOr("authoredscenario",0)==0 ||
            !tnt::Scenario::parse(text).hasScenario) return false;
        const auto crt=vfs.tryRead(vpath::replaceExtension(mapPath,".crt"));
        return crt && tak::crt::parse(*crt).version==1;
    } catch (const std::exception&) { return false; }
}

bool authoredScenario(const Package& package) {
    if (!package.files) return false;
    hpi::Vfs view;view.setMapFiles(package.files);
    return authoredScenario(view,package.mapPath);
}

sim::ScenarioInfo scenarioInfo(const Package& package, const sim::TypeRegistry* reg) {
    if (!package.files) return {};
    hpi::Vfs view; view.setMapFiles(package.files);
    return sim::inspectScenario(view, package.mapPath, reg);
}

std::shared_ptr<Package> build(const hpi::Vfs& vfs, const std::string& mapId) {
    std::string path = hpi::findMap(vfs, mapId);
    if (path.empty() || mapgen::isGeneratedMapId(path)) throw std::runtime_error("map not installed: " + mapId);
    path = hpi::MountSet::key(path);
    if (auto cached = vfs.cachedMap(path)) {
        auto files = std::make_shared<hpi::Vfs::Files>(*cached->second);
        return decode(encode(cached->first, std::move(files))->bytes, "");
    }
    return buildUncached(vfs, path);
}
std::shared_ptr<Package> decode(std::vector<uint8_t> bytes, const std::string& digest, tak::StopToken stop) {
    if(stop.stop_requested())throw std::runtime_error("map validation cancelled");
    if (bytes.empty() || bytes.size() > kMaxBytes) throw std::runtime_error("invalid map package size");
    const auto actual = hash(bytes);
    if (!digest.empty() && actual != digest) throw std::runtime_error("map checksum mismatch");
    Reader r(bytes.data(), bytes.size());
    if (r.u32() != 2) throw std::runtime_error("unsupported map package version");
    const auto path = r.str();
    const auto companions = companionPaths(path);
    if (!safePath(path) || !(path.starts_with("maps/") || path.starts_with("kmap/")) || !path.ends_with(".tnt"))
        throw std::runtime_error("invalid map path");
    auto files = std::make_shared<hpi::Vfs::Files>();
    const auto count = r.u32();
    if (count < 1 || count > 32768) throw std::runtime_error("invalid map file count");
    for (uint32_t i = 0; i < count; ++i) {
        if(stop.stop_requested())throw std::runtime_error("map validation cancelled");
        auto name = r.str(); const auto size = r.u32();
        const bool terrain = name.size() == 20 && name.starts_with("terrain/") && name.ends_with(".jpg") &&
            name.substr(8,8).find_first_not_of("0123456789abcdef") == std::string::npos;
        const bool feature = name.starts_with("features/") && name.ends_with(".tdf");
        const bool art = (name.starts_with("anims/") && (name.ends_with(".gaf") || name.ends_with(".taf"))) ||
                         (name.starts_with("palettes/") && name.ends_with(".pcx"));
        const bool companion = companions.count(name) != 0;
        // The map's AI profile (aiProfilePaths); which name it may use is checked below.
        const bool ai = name.starts_with("ai/") && name.ends_with(".txt") && name.find('/', 3) == std::string::npos;
        // Empty descriptions/restrictions are meaningful; geometry and art are not.
        if (!r.ok || !safePath(name) || (name != path && !companion && !terrain && !feature && !art && !ai) ||
            !r.avail(size) || (!size && !companion) || files->count(name)) throw std::runtime_error("invalid map resource");
        (*files)[name] = std::vector<uint8_t>(r.p, r.p + size); r.p += size;
    }
    if (!r.ok || r.p != r.end || !files->count(path)) throw std::runtime_error("incomplete map package");
    const auto& data = files->at(path);
    // Bound dimensions before allocating/parsing a remote TNT.
    Reader header(data.data(), data.size()); header.u32(); auto width = header.u32(), height = header.u32();
    if (!header.ok || width < 2 || height < 2 || width > 4096 || height > 4096 || width % 2 || height % 2)
        throw std::runtime_error("invalid map dimensions");
    auto map = tnt::Map::load(data, path);
    std::set<std::string> required{path};
    for (const auto& companion : companions)
        if (files->count(companion)) required.insert(companion);
    for (const auto& ai : aiProfilePaths(*files, path))
        if (files->count(ai)) required.insert(ai);
    for (auto key : map.tileKeys) required.insert(tilePath(key));
    hpi::Vfs view; view.setMapFiles(files);
    const auto features = featureResources(map, view, stop);
    required.insert(features.begin(), features.end());
    if (required.size() != files->size()) throw std::runtime_error("unreferenced map resources");
    for (const auto& name : required) if (!files->count(name)) throw std::runtime_error("missing map resource: " + name);
    auto p = std::make_shared<Package>(); p->mapPath = path; p->digest = actual;
    p->files = std::move(files); p->bytes = std::move(bytes); return p;
}
Writer offer(uint32_t room, const std::string& mapId, const Package& p) {
    Writer w; w.u32(room); w.str(mapId); w.str(p.digest); w.u32(uint32_t(p.bytes.size())); return w;
}
void Receiver::begin(uint32_t roomId, uint32_t xferId, uint32_t count, const std::string& h) {
    if (!roomId || !count || count > kMaxBytes || !validDigest(h)) throw std::runtime_error("invalid map offer");
    room = roomId; xfer = xferId; size = count; digest = h; bytes.clear();
}
Append Receiver::append(Reader& r) {
    auto id = r.u32(), x = r.u32(), off = r.u32(); const size_t n = size_t(r.end - r.p);
    if (!r.ok) throw std::runtime_error("invalid map chunk");
    if (!size || !room || id != room || x != xfer) return Append::Stale;
    if (off != bytes.size() || !n || n > kChunkBytes || n > size - bytes.size())
        throw std::runtime_error("invalid map chunk");
    bytes.insert(bytes.end(), r.p, r.end); return complete() ? Append::Done : Append::More;
}
void Sender::pump(Conn& conn, Msg kind) {
    while (package && conn.txPending() < 2 * kChunkBytes) {
        auto n = std::min(kChunkBytes, package->bytes.size() - offset);
        Writer w; w.u32(room); w.u32(xfer); w.u32(uint32_t(offset));
        w.b.insert(w.b.end(), package->bytes.begin() + offset, package->bytes.begin() + offset + n);
        conn.send(kind, w); offset += n;
        if (offset == package->bytes.size()) { package.reset(); offset = 0; }
    }
}
void saveCache(const std::filesystem::path& root, const Package& p,uint64_t quota) {
    if (!validDigest(p.digest)) throw std::runtime_error("invalid map cache key");
    const auto canonical=root / "MapCache" / (p.digest+".takmap");
    const auto archive=root / "MapCache" / (p.digest+".kmp");
    const bool needCanonical=!std::filesystem::exists(canonical),needArchive=!std::filesystem::exists(archive);
    if(!needCanonical && !needArchive)return;
    std::vector<uint8_t> packed;
    const auto usage=quota ? tak::storageUsage(root/"MapCache")+tak::storageUsage(root/"Maps","Generated-") : 0;
    if(quota)tak::storageRoom(usage,needCanonical?p.bytes.size():0,quota);
    if(needArchive) {
        std::vector<hpi::PackFile> files;
        for(const auto& [path,bytes]:*p.files)files.push_back({path,bytes});
        packed=hpi::pack(files);
    }
    tak::storageRoom(usage,(needCanonical?p.bytes.size():0)+packed.size(),quota);
    if(needCanonical)writeAtomic(canonical,p.bytes);
    if(needArchive)writeAtomic(archive,packed);
}
std::shared_ptr<Package> loadCache(const std::filesystem::path& root, const std::string& digest, size_t maxBytes, size_t expected, tak::StopToken stop) {
    if (!validDigest(digest)) return {};
    const auto path = root / "MapCache" / (digest + ".takmap");
    std::error_code ec; auto size = std::filesystem::file_size(path, ec);
    if (ec || !size || size > kMaxBytes) return {};
    if(size>maxBytes || (expected && size!=expected))throw std::runtime_error("cached map size/budget mismatch");
    try {
        std::vector<uint8_t> bytes(size); std::ifstream in(path, std::ios::binary);
        in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size));
        if (!in) return {};
        return decode(std::move(bytes), digest, stop);
    } catch (const std::exception&) { if(stop.stop_requested())return {}; std::error_code ec; std::filesystem::remove(path, ec); return {}; }
}
std::filesystem::path saveGenerated(const std::filesystem::path& root, const hpi::Vfs& vfs, const std::string& recipe,uint64_t quota) {
    if (!mapgen::isGeneratedMapId(recipe)) return {};
    const auto params = mapgen::parseMapId(recipe);   // an unknown recipe saves nothing
    const std::string id = crypto::toHex(crypto::sha256(recipe));
    const std::string name = params.name.empty() ? "Generated-" + std::string(mapgen::layoutName(params.layout)) + "-" +
        mapgen::friendlyLabel(params) + "-" + id.substr(0,16) : params.name + "-" + id.substr(0,16);
    const auto output = root / "Maps" / ("Generated-" + (params.name.empty() ? std::string{} : params.name + "-") + id + ".kmp");
    const std::string path = "kmap/" + name;
    if (std::filesystem::exists(output)) return output;
    const auto usage=quota ? tak::storageUsage(root/"MapCache")+tak::storageUsage(root/"Maps","Generated-") : 0;
    tak::storageRoom(usage,1,quota);
    auto g = mapgen::generate(params, vfs);
    const std::string world = mapgen::worldKey(params.mapType);
    std::string kingdom = world;
    kingdom[0] = char(std::toupper(uint8_t(kingdom[0])));
    // The radar pictures the map lists and the in-game minimap show, as the Cartographer builds them.
    terrain::Compositor compositor(vfs);
    terrain::generateMinimaps(g.map, compositor, terrain::loadWorldPalette(vfs, world));
    std::vector<hpi::PackFile> files{{path + ".tnt", g.map.save()}};
    std::string ota = "[GlobalHeader]\n{\nmissionname=" + name + ";\nmissiondescription=" +
        mapgen::friendlyLabel(params) + " Seed " + std::to_string(params.seed) + ";\nkingdom=" + kingdom +
        ";\nnumplayers=" + std::to_string(g.starts.size()) + ";\nsize=" + std::to_string(g.map.width / 32) + " x " +
        std::to_string(g.map.height / 32) + ";\ngravity=112;\nminwindspeed=100;\nmaxwindspeed=2000;\n[Map Data]\n{\n[Specials]\n{\n";
    for (size_t i = 0; i < g.starts.size(); ++i)
        ota += "[Special" + std::to_string(i) + "]\n{\nspecialwhat=StartPos" + std::to_string(i + 1) +
            ";\nxpos=" + std::to_string(g.starts[i].first) + ";\nzpos=" + std::to_string(g.starts[i].second) + ";\n}\n";
    ota += "}\n}\n}\n";
    files.push_back({path + ".ota", {ota.begin(), ota.end()}});
    files.push_back({path + ".recipe", {recipe.begin(), recipe.end()}});
    for (auto key : std::set<uint32_t>(g.map.tileKeys.begin(), g.map.tileKeys.end())) {
        auto tile = tilePath(key); files.push_back({tile, vfs.read(tile, g.map.stockTerrain)});
    }
    const auto packed=hpi::pack(files);
    tak::storageRoom(usage,packed.size(),quota);
    writeAtomic(output,packed); return output;
}
}
