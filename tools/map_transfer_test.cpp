#include "net/mappackage.h"
#include "net/client.h"
#include "net/netcompat.h"
#include "sim/matchsetup.h"
#include "tnt/mapgen.h"
#include "tnt/tnt.h"
#include "tnt/ota.h"
#include "sim/scenario.h"
#include "sim/scenarioinfo.h"
#include "crt/crt.h"
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <functional>
#include "util/virtualpath.h"
#include <iostream>
#include <map>
#include <thread>

using namespace tak;
static void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
template<class F> static void rejects(F f, const char* why) {
    bool failed = false; try { f(); } catch (const std::exception&) { failed = true; } check(failed, why);
}
static void write(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}
// Every entry under `root` (symlinks not followed) with its size and write time:
// equal before and after a run proves nothing in that data folder was written.
static std::map<std::string,std::pair<uintmax_t,std::filesystem::file_time_type>> tree(const std::filesystem::path& root) {
    std::map<std::string,std::pair<uintmax_t,std::filesystem::file_time_type>> out;
    for(const auto& e:std::filesystem::recursive_directory_iterator(root)) {
        std::error_code ec;const bool file=e.is_regular_file(ec) && !e.is_symlink(ec);
        out[e.path().string()]={file?e.file_size(ec):0,std::filesystem::symlink_status(e.path()).type()==std::filesystem::file_type::symlink?std::filesystem::file_time_type{}:e.last_write_time(ec)};
    }
    return out;
}
static void readOnly(const std::filesystem::path& dir) {
    using std::filesystem::perms;
    std::filesystem::permissions(dir,perms::owner_write|perms::group_write|perms::others_write,std::filesystem::perm_options::remove);
}
static int soloNetwork(uint16_t port,const char* hostRoot,const char* peerRoot) {
    auto original=hpi::mountRetailRoot(hostRoot);
    const auto stock=net::maps::build(original,"Ulasem Arena");
    const auto starts=sim::parseStartPositions(original,stock->mapPath);
    check(!starts.empty(),"solo fixture needs a start");
    crt::Scenario scenario;scenario.players.resize(9);
    crt::Unit monarch;monarch.objectName="araking";monarch.player=0;
    monarch.x=int(starts[0].first/16);monarch.z=int(starts[0].second/16);
    monarch.health=75;monarch.armor=150;monarch.weapon=175;monarch.veteran=3;
    monarch.uniqueName="Network monarch with a deliberately long display name";
    monarch.y=999;
    scenario.units.push_back(monarch);
    auto neutral=monarch;neutral.player=8;neutral.x+=10;neutral.uniqueName="Neutral monarch";
    scenario.units.push_back(neutral);
    scenario.customTypes.push_back({"araking",{100,200,200,2}});
    scenario.regions.push_back({"Reinforcements",monarch.x-6,monarch.z-6,monarch.x+6,monarch.z+6});
    scenario.regions.push_back({"Rally",monarch.x+4,monarch.z+4,monarch.x+6,monarch.z+6});
    scenario.players[1].push_back({{{0,{}}},{{13,{"Player 1","Solo authored network"}},
        {16,{"500"}},{17,{"123"}},{7,{"ARAARCH","Reinforcements"}},
        {11,{"ARAARCH","7","Anywhere"}},
        {15,{"ARAARCH","Anywhere","Rally"}},{14,{}}}});
    scenario.players[1].push_back({{{1,{"3"}}},{{20,{}},{14,{}}}});
    scenario.players[1].push_back({{{1,{"8"}}},{{5,{}},{14,{}}}});
    std::vector<hpi::PackFile> entries;
    const auto stem=stock->mapPath.substr(0,stock->mapPath.size()-4);
    for(const auto& [name,data]:*stock->files) {
        const auto path=name.starts_with(stem+".")?"kmap/solo authored"+name.substr(stem.size()):name;
        if(path=="kmap/solo authored.crt" || path=="kmap/solo authored.tdf")continue;
        if(path=="kmap/solo authored.ota") {
            auto metadata=tnt::Scenario::parse(std::string(data.begin(),data.end()));
            metadata.hasScenario=true;metadata.useOnlyUnits="solo authored.tdf";
            const auto text=metadata.write()+"\n[TAKPlaytest]{\nauthoredscenario=1;\n}\n";
            entries.push_back({path,{text.begin(),text.end()}});
        } else entries.push_back({path,data});
    }
    entries.push_back({"kmap/solo authored.crt",crt::write(scenario)});
    const std::string restriction="[ARAKING]{}\n";
    entries.push_back({"kmap/solo authored.tdf",{restriction.begin(),restriction.end()}});
    write(std::filesystem::path(hostRoot)/"Maps"/"solo-authored.kmp",hpi::pack(entries));
    auto hostData=hpi::mountRetailRoot(hostRoot);
    auto peerData=hpi::mountRetailRoot(peerRoot);
    net::MpClient host,observer;host.setMapRoot(hostRoot);observer.setMapRoot(peerRoot);
    host.setDataHash(hpi::gameplayHash(hostData));observer.setDataHash(hpi::gameplayHash(peerData));
    check(host.connect("127.0.0.1",port,"solo-author"),"solo host connect");
    sim::TypeRegistry registry;sim::setupRegistry(registry,hostData,true);
    sim::World wh,wo;
    bool created=false,ready=false,started=false,loaded=false,connected=false,spectating=false,observerLoaded=false,sent=false;
    uint32_t ht=0,ot=0;std::map<uint32_t,uint64_t> hashes;
    auto setup=[&](net::MpClient& client,hpi::Vfs& base,sim::World& world) {
        check(client.mapPackage() && net::maps::authoredScenario(*client.mapPackage()),"verified authored package missing");
        hpi::Vfs view(&base);view.setMapFiles(client.mapPackage()->files);
        sim::MatchConfig cfg;cfg.vfs=&view;cfg.mapPath=client.mapPackage()->mapPath;
        cfg.startSeed=client.startSeed();cfg.startMode=sim::StartMode(net::normalizeStartMode(client.startRoom().opts.randomStarts));
        cfg.unitCap=client.startRoom().opts.unitCap;cfg.slots={{true,0,0,1,false,false}};
        sim::setupMatch(world,registry,cfg);
        check(world.numPlayers()==1 && world.units().size()==3,"scripted reinforcement missing or neutral became a lobby player");
        check(world.unit(1)->scenarioArmor>2.99f && world.unit(1)->scenarioArmor<3.01f &&
              world.unit(1)->scenarioWeapon>3.49f && world.unit(1)->scenarioWeapon<3.51f,
              "authored stat multipliers lost on network participant");
        check(!world.buildAllowed(registry.find("tarnecro")) && !world.buildAllowed(registry.find("araarch")) &&
              world.buildAllowed(registry.find("araking")),"transferred Use Only not installed");
        check(world.unit(1)->scenarioName==monarch.uniqueName.substr(0,31),"named placement lost on network participant");
        check(world.player(0).scenarioResourceLimit==500 && world.player(0).mana==123,"startup resource actions lost on network participant");
        const auto* reinforcement=world.unit(3);
        check(reinforcement && reinforcement->type==registry.find("araarch") &&
              reinforcement->hp.floorInt()==reinforcement->type->maxHp-7,
              "scripted create bypass or raw HP damage failed on network participant");
        check(!reinforcement->orders.empty(),"scripted reinforcement move was not queued");
        client.reportLoaded(hpi::gameplayHash(base));
    };
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(90);
    while(std::chrono::steady_clock::now()<until) {
        check(host.poll(),host.error().c_str());
        if(!created && host.state()==net::MpClient::State::Lobby) {
            net::GameOptions opts;opts.crusades=1;opts.gameMods=0;
            host.createGame("solo authored test","","Solo Authored",opts,2);created=true;
        }
        if(!ready && host.room().id) {host.setSlot(0,1,0,0,0,1);ready=true;}
        if(!started && ready && host.room().mapsReady && host.room().slots[0].ready) {
            check(host.room().slots[1].type!=1 && host.room().slots[1].type!=2,"solo fixture accidentally seated opponent");
            host.startGame();started=true;
        }
        if(!loaded && host.starting()) {setup(host,hostData,wh);loaded=true;}
        if(loaded && ht>30 && !connected) {check(observer.connect("127.0.0.1",port,"solo-observer"),"solo observer connect");connected=true;}
        if(connected) {
            check(observer.poll(),observer.error().c_str());
            if(!spectating && observer.state()==net::MpClient::State::Lobby) {observer.spectate(host.room().id,"");spectating=true;}
            if(!observerLoaded && (observer.starting() || observer.isCatchUp())) {setup(observer,peerData,wo);observerLoaded=true;}
        }
        if(loaded && ht>30 && !sent) {
            net::Command command;command.player=7;command.unitId=1;command.kind=net::Cmd::Train;
            std::strcpy(command.type,"tarnecro");
            auto repeat=command;repeat.kind=net::Cmd::RepeatTrain;
            auto build=command;build.kind=net::Cmd::Build;build.x=400;build.z=400;
            host.sendCommands({command,repeat,build});sent=true;
        }
        auto advance=[&](net::MpClient& client,sim::World& world,uint32_t& tick,bool reference) {
            net::Bundle bundle;
            while((reference || hashes.count(tick)) && client.takeBundle(tick,bundle)) {
                for(const auto& c:bundle.cmds)sim::applyCommand(world,registry,c);
                for(const auto& e:bundle.events)sim::applyEvent(world,e);
                world.tick(1.f/30.f);
                if(tick<90)check(world.player(0).scenarioResourceLimit==500 && world.player(0).mana==123,
                                 "resource limit did not suppress natural income during network replay");
                const auto hash=world.stateHash();
                if(reference)hashes[tick]=hash;else check(hashes.at(tick)==hash,"solo observer replay diverged");
                if(!client.isSpectator() && tick%net::kHashPeriod==0)client.sendHash(tick,hash);
                ++tick;
            }
        };
        if(loaded)advance(host,wh,ht,true);
        if(observerLoaded)advance(observer,wo,ot,false);
        check(!host.desynced() && !observer.desynced(),"solo referee hash mismatch");
        // CRT results are per-player deterministic world state, not the campaign's
        // broadcast MissionOutcome message. Continue beyond the result so the
        // referee receives/checks post-result periodic hashes as well.
        if(observerLoaded && ht>=330 && wh.scenarioOutcome(0)==1 && wo.scenarioOutcome(0)==1 && ot==ht) {
            check(ht>240 && sent,"solo result occurred before authored victory condition");
            check(wh.scenarioOutcome(0)==1 && wo.scenarioOutcome(0)==1,"server result differs from scenario");
            check(wh.unit(1)->buildQueue.empty() && wh.units().size()==3,"network construction bypassed Use Only");
            check(wh.player(0).scenarioResourceLimit==0 && wo.player(0).scenarioResourceLimit==0 && wh.player(0).mana>123,
                  "timed Resources Normal did not restore income across peers");
            check(wo.unit(1)->scenarioName==monarch.uniqueName.substr(0,31),"late observer lost authored display name");
            std::cout<<"PASS: solo authored names, neutral/stats/restrictions, scripted create/raw HP/move/resource reset, host/referee/late observer parity, victory verified after "<<ht<<" ticks hash "<<std::hex<<wh.stateHash()<<'\n';
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("solo authored network timed out: "+host.mapStatus()+"; "+host.error());
}

// A scripted loopback server for one MpClient (as spectator_client_test's): the client's
// side of room editing and of the transfer ids, without takserver.
struct FakeServer {
    int listener = -1; uint16_t port = 0; net::Conn conn;
    FakeServer() {
        std::string error; listener = net::listenOn(0, error, true);
        if (listener < 0) throw std::runtime_error(error);
        sockaddr_storage address{}; socklen_t n = sizeof address;
        if (getsockname(listener, reinterpret_cast<sockaddr*>(&address), &n)) throw std::runtime_error("getsockname");
        port = address.ss_family == AF_INET ? ntohs(reinterpret_cast<sockaddr_in*>(&address)->sin_port)
                                            : ntohs(reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);
    }
    ~FakeServer() { if (listener >= 0) net::sockClose(listener); }
    void accept(net::MpClient& client) {
        for (auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2); std::chrono::steady_clock::now() < end;) {
            client.poll();
            sockaddr_storage address{}; socklen_t n = sizeof address;
            int fd = int(::accept(listener, reinterpret_cast<sockaddr*>(&address), &n));
            if (fd >= 0) { net::setupSocket(fd); conn = net::Conn(fd); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        throw std::runtime_error("timed out accepting the client");
    }
    // Send the frames and let the client poll a few times (it answers a Ping inside the
    // poll that reads it, before that poll's uploads go out), then a Ping: return what the
    // client sent before its Pong (keepalives skipped), everything it did about them.
    std::vector<net::Frame> exchange(net::MpClient& client, std::vector<std::pair<net::Msg, net::Writer>> frames = {}) {
        for (const auto& [kind, w] : frames) conn.send(kind, w);
        std::vector<net::Frame> out;
        bool pinged = false;
        for (auto end = std::chrono::steady_clock::now() + std::chrono::seconds(3); std::chrono::steady_clock::now() < end;) {
            for (int i = 0; i < 10; ++i) {
                conn.flushWrite(); client.poll();
                if (!conn.recv()) throw std::runtime_error("fake server receive");
                net::Frame f;
                while (conn.poll(f)) {
                    if (f.kind == net::Msg::Pong && pinged) return out;
                    if (f.kind == net::Msg::Pong) continue;
                    if (f.kind == net::Msg::Ping) { conn.send(net::Msg::Pong); continue; }
                    out.push_back(f);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!pinged) { conn.send(net::Msg::Ping); pinged = true; }
        }
        throw std::runtime_error("timed out waiting for the client to process a message");
    }
};
static const net::Frame* findFrame(const std::vector<net::Frame>& frames, net::Msg kind) {
    for (const auto& f : frames) if (f.kind == kind) return &f;
    return nullptr;
}

// MpClient's side of protocol 323: the host's map transfer follows the server's MapRequest
// (digest and xfer), and setRoomMap changes nothing until its serial is answered.
static void clientRoomEdits(const std::filesystem::path& root, const net::maps::Package& package) {
    std::vector<hpi::PackFile> files;
    for (const auto& [path, bytes] : *package.files) files.push_back({path, bytes});
    write(root / "data" / "Maps" / "edit-test.kmp", hpi::pack(files));
    FakeServer server; net::MpClient client;
    client.setMapRoot(root / "data"); client.setCacheRoot(root / "cache");
    check(client.connect("127.0.0.1", server.port, "edit-host"), "fake server connect");
    server.accept(client);
    check(findFrame(server.exchange(client), net::Msg::Hello) != nullptr, "hello");
    { net::Writer w; w.u32(5); w.str("edit-host"); w.u8(0); server.exchange(client, {{net::Msg::Welcome, w}}); }
    check(client.state() == net::MpClient::State::Lobby, "fake server: in the lobby");
    net::GameOptions opts; opts.gameMods = 0;
    // A name the server would refuse (not printable ASCII, past kRoomNameMax) is sent as
    // the part it takes.
    client.createGame("edit f\xc3\xbcr alle " + std::string(40, 'x'), "", "test", opts, 2);
    const auto created = server.exchange(client);
    const auto* create = findFrame(created, net::Msg::CreateGame);
    check(create != nullptr, "create request");
    net::Reader cr(create->payload.data(), create->payload.size());
    const auto sentName = cr.str();
    check(sentName == "edit fr alle " + std::string(net::kRoomNameMax - 13, 'x'), "the room name is printable ASCII, 32 bytes");
    auto lobby = [&](const std::string& mapId, uint32_t rev, uint32_t serial) {
        net::RoomState r; r.id = 9; r.name = "edit"; r.mapId = mapId; r.hostId = 5; r.capacity = 2;
        r.roomRev = rev; r.mapSerial = serial; r.slots[0].type = 1; r.slots[0].name = "edit-host";
        for (int i = 2; i < net::kMaxSlots; ++i) r.slots[i].type = 3;
        net::Writer w; net::put(w, r); return w;
    };
    auto joinResult = [](uint8_t slot, uint32_t rev) { net::Writer w; w.u8(1); w.u8(slot); w.str(""); w.u32(rev); return w; };
    auto mapRequest = [](const std::string& digest, uint32_t xfer) { net::Writer w; w.u32(9); w.str(digest); w.u32(xfer); return w; };
    auto out = server.exchange(client, {{net::Msg::JoinResult, joinResult(0, 1)}, {net::Msg::LobbyState, lobby("test", 1, 0)}});
    const auto* offer = findFrame(out, net::Msg::MapOffer);
    check(offer != nullptr, "the host offers its room's map");
    net::Reader offered(offer->payload.data(), offer->payload.size());
    offered.u32(); const auto offeredMap = offered.str(), digest = offered.str();
    check(offeredMap == "test" && digest == client.mapPackage()->digest, "the offer is the room's map");
    // A MapRequest for another package (the map changed since it was sent) is stale.
    out = server.exchange(client, {{net::Msg::MapRequest, mapRequest(std::string(64, '0'), 3)}});
    check(!findFrame(out, net::Msg::MapChunk) && !findFrame(out, net::Msg::MapError) && client.error().empty(),
          "a stale MapRequest at the host starts no upload and is no error");
    // The current one starts the upload with its xfer; a newer one starts it over from 0.
    auto firstChunk = [&](const std::vector<net::Frame>& frames, uint32_t& xfer, uint32_t& offset) {
        const auto* chunk = findFrame(frames, net::Msg::MapChunk);
        check(chunk != nullptr, "the host uploads its package");
        net::Reader r(chunk->payload.data(), chunk->payload.size());
        check(r.u32() == 9, "a chunk names its room"); xfer = r.u32(); offset = r.u32();
    };
    uint32_t xfer = 0, offset = 1;
    firstChunk(server.exchange(client, {{net::Msg::MapRequest, mapRequest(digest, 3)}}), xfer, offset);
    check(xfer == 3 && offset == 0, "the upload carries the request's xfer, from offset 0");
    firstChunk(server.exchange(client, {{net::Msg::MapRequest, mapRequest(digest, 4)}}), xfer, offset);
    check(xfer == 4 && offset == 0, "a newer request starts the upload over from offset 0");

    // setRoomMap: nothing changes when it is sent; a refusal leaves everything as it was.
    const auto recipe = mapgen::encodeMapId(mapgen::Params{});
    const auto before = client.mapPackage();
    check(client.roomEditable() && client.setRoomMap(recipe, 2, false, 0) && client.roomMapPending() == 1, "a map change is sent");
    check(!client.setRoomMap(recipe, 2, false, 0) && !client.roomError().empty(), "one map change at a time");
    out = server.exchange(client);
    {
        const auto* request = findFrame(out, net::Msg::SetRoomMap);
        check(request != nullptr, "SetRoomMap sent");
        net::Reader r(request->payload.data(), request->payload.size());
        const uint32_t serial = r.u32(); const auto mapId = r.str();
        check(serial == 1 && mapId == recipe && r.u8() == 2 && r.u8() == 0 && r.u8() == 0 && r.ok && r.p == r.end,
              "SetRoomMap: serial, map, capacity, scenario, start mode");
    }
    check(client.room().mapId == "test" && client.mapPackage() == before, "nothing changes before the answer");
    {
        net::Writer refused; refused.u8(uint8_t(net::RoomRequest::Map)); refused.u32(1); refused.str("not this one");
        server.exchange(client, {{net::Msg::RoomError, refused}, {net::Msg::LobbyState, lobby("test", 1, 1)}});
    }
    check(client.roomMapAnswer().serial == 1 && client.roomMapAnswer().kind == net::MpClient::RoomMapAnswer::Refused &&
          client.roomMapAnswer().why == "not this one" && client.roomMapPending() == 0 &&
          client.roomError() == "not this one" && client.roomErrorFor() == net::RoomRequest::Map,
          "a refused map change is answered, with the reason");
    check(client.room().mapId == "test" && client.mapPackage() == before && client.error().empty(),
          "a refusal changes no transfer state");
    // The next one is numbered past what the room answered, and its answer is the LobbyState
    // carrying its serial.
    check(client.setRoomMap(recipe, 2, false, 0) && client.roomMapPending() == 2 && client.roomError().empty(),
          "the next serial follows the room's");
    server.exchange(client);
    const uint64_t seq = client.lobbySeq();
    server.exchange(client, {{net::Msg::LobbyState, lobby(recipe, 2, 2)}});
    check(client.lobbySeq() == seq + 1, "lobbySeq counts LobbyStates");
    check(client.roomMapAnswer().serial == 2 && client.roomMapAnswer().kind == net::MpClient::RoomMapAnswer::Accepted &&
          client.roomMapPending() == 0 && client.room().mapId == recipe && client.mapStatus() == "MAP READY" &&
          client.room().roomRev == 2, "an accepted map change becomes the room's map");
    // The same map, scenario and start mode: nothing to ask.
    check(client.setRoomMap(recipe, 2, false, 0) && client.roomMapPending() == 0 && !findFrame(server.exchange(client), net::Msg::SetRoomMap),
          "an unchanged map is not asked for");
    check(!client.setRoomMap("No Such Map", 2, false, 0) && !client.roomError().empty() && client.roomMapPending() == 0,
          "a map this host cannot build is refused here");
    // Back to the file map: built into a pending package, offered only once answered.
    check(client.setRoomMap("test", 2, false, 0) && client.roomMapPending() == 3, "file map change sent");
    out = server.exchange(client, {{net::Msg::LobbyState, lobby(recipe, 2, 2)}});
    check(client.roomMapPending() == 3 && client.room().mapId == recipe && !findFrame(out, net::Msg::MapOffer),
          "another LobbyState is not the answer, and offers nothing");
    out = server.exchange(client, {{net::Msg::LobbyState, lobby("test", 3, 3)}});
    offer = findFrame(out, net::Msg::MapOffer);
    check(client.roomMapAnswer().kind == net::MpClient::RoomMapAnswer::Accepted && client.roomMapPending() == 0 &&
          offer != nullptr && client.mapPackage() && client.mapPackage()->digest == digest,
          "the confirmed file map's pending package is offered");
    // WATCH: the server's JoinResult 0xFF opens the host's seat in the client's table too.
    client.watch();
    out = server.exchange(client);
    {
        const auto* move = findFrame(out, net::Msg::MoveSeat);
        check(move && move->payload.size() == 1 && move->payload[0] == net::kWatchSeat, "watch asks for kWatchSeat");
    }
    server.exchange(client, {{net::Msg::JoinResult, joinResult(net::kWatchSeat, 4)}});
    check(client.isSpectator() && client.room().mySlot == -1 && client.room().slots[0].type == 0 &&
          client.room().slots[0].name.empty() && client.room().roomRev == 4, "watching: the old seat opens");
    // A START pending: the room is not editable.
    client.startGame();
    check(!client.roomEditable() && !client.setRoomMap(recipe, 2, false, 0) && !client.roomError().empty() &&
          !findFrame(server.exchange(client), net::Msg::SetRoomMap), "no map change once START is pending");
    std::cout << "room editing (client): stale and newer MapRequests, refused/accepted/unchanged map changes, pending package, watch, START lock passed\n";
}

// A map only `root` has: a shipped map's package under a new name (its own digest, so no
// server or guest holds it until it is sent).
static void hostOnlyMap(const std::filesystem::path& root, const hpi::Vfs& data, const std::string& source,
                        const std::string& stem) {
    const auto package = net::maps::build(data, source);
    const auto from = package->mapPath.substr(0, package->mapPath.size() - 4);
    std::vector<hpi::PackFile> entries;
    for (const auto& [name, bytes] : *package->files)
        entries.push_back({name.starts_with(from + ".") ? "kmap/" + stem + name.substr(from.size()) : name, bytes});
    write(root / "Maps" / (stem + ".kmp"), hpi::pack(entries));
}

// --room-edit: protocol 323's map changes between real MpClients through a real takserver
// (run by server_room_edit_test.py, which checks the server's log afterwards). Each case
// is a fresh room of fresh connections.
static int roomEdit(uint16_t port, const std::filesystem::path& hostRoot, const std::filesystem::path& peerRoot,
                    const std::filesystem::path& thirdRoot) {
    using S = net::MpClient::State;
    using Answer = net::MpClient::RoomMapAnswer;
    auto base = hpi::mountRetailRoot(peerRoot);
    const std::string A = "Room Edit A", B = "Room Edit B", C = "Room Edit C", KOTH = "King of the Hill", INNER = "Inner Circle";
    hostOnlyMap(hostRoot, base, INNER, "room edit a");    // ~6 MB: the server must take an upload
    hostOnlyMap(hostRoot, base, KOTH, "room edit b");     // small: guests download it
    hostOnlyMap(hostRoot, base, "Ulasem Arena", "room edit c");
    const auto cacheBase = std::filesystem::temp_directory_path() / ("tak-room-edit-" + crypto::toHex(crypto::randomVec(8)));
    struct CleanCache { std::filesystem::path p; ~CleanCache() { std::error_code ec; std::filesystem::remove_all(p, ec); } } clean{cacheBase};
    sim::TypeRegistry reg; sim::setupRegistry(reg, base, true);
    int made = 0;
    std::map<const net::MpClient*, std::vector<std::string>> history;   // each client's map status changes
    auto client = [&](const std::filesystem::path& root, const std::string& name) {
        auto c = std::make_unique<net::MpClient>();
        c->setMapRoot(root); c->setCacheRoot(cacheBase / (name + std::to_string(++made)));   // nothing downloaded yet
        c->setDataHash(hpi::gameplayHash(hpi::mountRetailRoot(root)));
        check(c->connect("127.0.0.1", port, name), "room edit connect");
        return c;
    };
    auto pump = [&](const std::vector<net::MpClient*>& clients, double seconds, const std::function<bool()>& done,
                    const std::string& what) {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(int(seconds * 1000));
        while (std::chrono::steady_clock::now() < end) {
            for (auto* c : clients) {
                if (!c->poll()) throw std::runtime_error(what + ": a client lost its connection: " + c->error());
                auto& seen = history[c];
                if (seen.empty() || seen.back() != c->mapStatus()) seen.push_back(c->mapStatus());
            }
            if (done()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        throw std::runtime_error(what + ": timed out");
    };
    auto saw = [&](const net::MpClient& c, const std::string& text) {
        const auto& seen = history[&c];
        return std::any_of(seen.begin(), seen.end(), [&](const std::string& s) { return s.find(text) != std::string::npos; });
    };
    // A room of `mapId` with the guests seated (slots 1, 2, ...), every client in it.
    auto open = [&](net::MpClient& host, const std::vector<net::MpClient*>& guests, const std::string& mapId, uint8_t cap) {
        std::vector<net::MpClient*> all{&host}; all.insert(all.end(), guests.begin(), guests.end());
        pump(all, 10, [&] { return std::all_of(all.begin(), all.end(), [](auto* c) { return c->state() == S::Lobby; }); }, "lobby");
        net::GameOptions o; o.gameMods = 0;
        host.createGame("room edit", "", mapId, o, cap);
        pump(all, 10, [&] { return host.room().id != 0; }, "room created");
        for (auto* g : guests) {
            g->joinGame(host.room().id, "");
            pump(all, 10, [&] { return g->state() == S::InRoom && g->room().id == host.room().id; }, "guest seated");
        }
        return all;
    };
    auto ready = [&](net::MpClient& c) {
        const int slot = c.room().mySlot;
        c.setSlot(slot, 1, uint8_t(slot % 5), uint8_t(slot), uint8_t(slot), 1);
    };
    auto readyAll = [&](const std::vector<net::MpClient*>& all) {
        for (auto* c : all) ready(*c);
        pump(all, 20, [&] {
            const auto& r = all[0]->room();
            return r.mapsReady && std::all_of(all.begin(), all.end(), [&](auto* c) {
                return c->room().mySlot >= 0 && r.slots[c->room().mySlot].ready && c->room().roomRev == r.roomRev; });
        }, "every player ready and every map verified");
    };
    // Ask for a map change and wait for its answer.
    auto change = [&](const std::vector<net::MpClient*>& all, const std::string& mapId, uint8_t cap, bool scenario,
                      Answer::Kind want, const std::string& what) {
        auto& host = *all[0];
        check(host.setRoomMap(mapId, cap, scenario, 0), (what + ": setRoomMap refused locally: " + host.roomError()).c_str());
        const uint32_t serial = host.roomMapPending();
        check(serial != 0, (what + ": nothing was sent").c_str());
        pump(all, 10, [&] { return host.roomMapAnswer().serial == serial; }, what);
        check(host.roomMapAnswer().kind == want && host.roomMapPending() == 0,
              (what + ": answered " + std::to_string(int(host.roomMapAnswer().kind)) + " " + host.roomMapAnswer().why).c_str());
    };
    auto noMapError = [&](const std::vector<net::MpClient*>& all, const std::string& what) {
        for (auto* c : all) check(!saw(*c, "MAP ERROR") && c->error().empty(), (what + ": a MAP ERROR").c_str());
    };
    auto bothStart = [&](const std::vector<net::MpClient*>& all, const std::string& what) {
        pump(all, 20, [&] { return std::all_of(all.begin(), all.end(), [](auto* c) { return c->starting() || c->state() == S::InGame; }); }, what);
    };

    {   // change map: the guest downloads the new map, is unready, and both play it.
        auto host = client(hostRoot, "edit-host"), guest = client(peerRoot, "edit-guest");
        auto all = open(*host, {guest.get()}, KOTH, 4);
        readyAll(all);
        const uint32_t rev = host->room().roomRev;
        change(all, B, 4, false, Answer::Accepted, "change map");
        pump(all, 10, [&] { return guest->room().mapId == B; }, "the guest sees the new map");
        check(host->room().roomRev > rev && (host->room().changeBits & net::kChangeMap) && !host->room().slots[1].ready,
              "a map change unreadies the guest");
        pump(all, 60, [&] { return host->room().mapsReady && guest->mapStatus() == "MAP VERIFIED"; }, "the new map verifies");
        check(saw(*guest, "DOWNLOADING MAP"), "the guest did not download the host's new map");
        noMapError(all, "change map");
        readyAll(all);
        host->startGame();
        bothStart(all, "change map: start");
        check(host->mapPackage() && guest->mapPackage() && host->mapPackage()->digest == guest->mapPackage()->digest &&
              host->startRoom().mapId == B && guest->startRoom().mapId == B, "both start on the new map");
        // Both load it and play 60 matching ticks.
        sim::World wh, wg; uint32_t th = 0, tg = 0; std::map<uint32_t, uint64_t> hashes;
        for (auto [c, world] : {std::pair{host.get(), &wh}, std::pair{guest.get(), &wg}}) {
            hpi::Vfs view(&base); view.setMapFiles(c->mapPackage()->files);
            sim::MatchConfig cfg; cfg.vfs = &view; cfg.mapPath = c->mapPackage()->mapPath;
            cfg.startSeed = c->startSeed(); cfg.startMode = sim::StartMode(net::normalizeStartMode(c->startRoom().opts.randomStarts));
            cfg.unitCap = c->startRoom().opts.unitCap;
            cfg.slots = {{true, 0, 0, 1, false, false}, {true, 1, 1, 1, false, false}};
            sim::setupMatch(*world, reg, cfg);
            c->reportLoaded(c->dataHash());
        }
        auto advance = [&](net::MpClient& c, sim::World& world, uint32_t& tick, bool reference) {
            net::Bundle bundle;
            while (tick < 60 && (reference || hashes.count(tick)) && c.takeBundle(tick, bundle)) {
                for (const auto& cmd : bundle.cmds) sim::applyCommand(world, reg, cmd);
                for (const auto& e : bundle.events) sim::applyEvent(world, e);
                world.tick(1.f / 30.f);
                const auto hash = world.stateHash();
                if (reference) hashes[tick] = hash; else check(hashes.at(tick) == hash, "change map: the worlds diverged");
                if (tick % net::kHashPeriod == 0) c.sendHash(tick, hash);
                ++tick;
            }
        };
        pump(all, 30, [&] { advance(*host, wh, th, true); advance(*guest, wg, tg, false); return th == 60 && tg == 60; },
             "change map: 60 ticks");
        std::cout << "change map: the guest downloaded the new map, readied again; 60 matching ticks on it\n";
    }
    {   // flip: A -> B -> A, each as soon as the last is answered.
        auto host = client(hostRoot, "flip-host"), guest = client(peerRoot, "flip-guest");
        auto all = open(*host, {guest.get()}, INNER, 4);
        readyAll(all);
        change(all, B, 4, false, Answer::Accepted, "flip to B");
        change(all, INNER, 4, false, Answer::Accepted, "flip back");
        pump(all, 60, [&] { return host->room().mapId == INNER && guest->room().mapId == INNER && host->room().mapsReady &&
                                   guest->room().mapsReady; }, "flip: verified on the first map");
        noMapError(all, "flip");
        std::cout << "flip: A -> B -> A ends verified on A, no MAP ERROR\n";
    }
    {   // mid-upload: the host is uploading a map the server lacks when it changes the map.
        auto host = client(hostRoot, "upload-host"), guest = client(peerRoot, "upload-guest");
        auto all = open(*host, {guest.get()}, A, 4);
        pump(all, 30, [&] { return host->mapStatus() == "UPLOADING MAP TO SERVER"; }, "mid-upload: the upload starts");
        check(!host->room().mapsReady, "mid-upload: the map verified before the change");
        change(all, KOTH, 4, false, Answer::Accepted, "mid-upload change");
        pump(all, 30, [&] { return host->room().mapId == KOTH && host->room().mapsReady && guest->room().mapsReady; },
             "mid-upload: the new map verifies");
        noMapError(all, "mid-upload");
        std::cout << "mid-upload: the change during an upload verifies the new map, no MAP ERROR\n";
    }
    {   // refused mid-upload: three seated humans refuse a 2-start map while the host uploads.
        auto host = client(hostRoot, "refused-host"), guest = client(peerRoot, "refused-guest"), third = client(thirdRoot, "refused-third");
        auto all = open(*host, {guest.get(), third.get()}, C, 4);
        pump(all, 30, [&] { return host->mapStatus() == "UPLOADING MAP TO SERVER"; }, "refused mid-upload: the upload starts");
        check(!host->room().mapsReady, "refused mid-upload: the map verified before the change");
        change(all, INNER, 2, false, Answer::Refused, "refused mid-upload");
        check(host->roomMapAnswer().why.find("3 players are seated") != std::string::npos, "refused mid-upload: the reason");
        pump(all, 60, [&] { return host->room().mapId == C && std::all_of(all.begin(), all.end(), [](auto* c) {
                                return c->room().mapsReady; }); }, "refused mid-upload: the map still verifies");
        noMapError(all, "refused mid-upload");
        std::cout << "refused mid-upload: refused while the host uploaded; the map still verifies\n";
    }
    {   // MAP SCENARIO on and off, then START.
        auto host = client(hostRoot, "scenario-host"), guest = client(peerRoot, "scenario-guest");
        auto all = open(*host, {guest.get()}, KOTH, 4);
        readyAll(all);
        change(all, KOTH, 4, true, Answer::Accepted, "scenario on");
        check(host->room().opts.mapScenario == 1 && !host->room().slots[1].ready, "scenario on: unready guest");
        change(all, KOTH, 4, false, Answer::Accepted, "scenario off");
        check(host->room().opts.mapScenario == 0, "scenario off");
        readyAll(all);
        host->startGame();
        bothStart(all, "scenario toggle then START");
        std::cout << "scenario toggle, then START: both start\n";
    }
    {   // A refused change, then START.
        auto host = client(hostRoot, "refusal-host"), guest = client(peerRoot, "refusal-guest");
        auto all = open(*host, {guest.get()}, KOTH, 4);
        readyAll(all);
        change(all, "~gen1~09000100000000000000038001800108ffffffffff0004", 2, false, Answer::Refused, "unknown recipe");
        check(host->room().slots[1].ready, "a refused change unreadied the guest");
        host->startGame();
        bothStart(all, "refused change then START");
        std::cout << "refused SetRoomMap, then START: both start\n";
    }
    {   // START, then a change before GameStarting: refused here, the game starts on the old map.
        auto host = client(hostRoot, "start-host"), guest = client(peerRoot, "start-guest");
        auto all = open(*host, {guest.get()}, KOTH, 4);
        readyAll(all);
        host->startGame();
        check(!host->roomEditable() && !host->setRoomMap(INNER, 4, false, 0) && host->roomMapPending() == 0,
              "a change after START was not refused locally");
        bothStart(all, "START then change");
        check(host->startRoom().mapId == KOTH && guest->startRoom().mapId == KOTH, "the game starts on the old map");
        std::cout << "START, then setRoomMap: refused locally; the game starts on the old map\n";
    }
    std::cout << "PASS: room editing between real clients\n";
    return 0;
}

int main(int argc, char** argv) try {
    if(argc>=4 && argc<=6 && std::string(argv[1])=="--export-package") {
        auto data=hpi::mountRetailRoot(argv[2]);
        const auto package=net::maps::build(data,argc>=5 ? argv[4] : "Ulasem Arena");
        if(argc==6) {
            // A crafted host's package: the same map with one companion (argv[5] is its
            // suffix, e.g. ".tdf") left out. decode accepts it -- companions are optional --
            // which is what server_map_scenario_test needs the server to catch at START.
            const auto stem=package->mapPath.substr(0,package->mapPath.size()-4);
            std::vector<std::pair<std::string,const std::vector<uint8_t>*>> kept;
            for(const auto& [name,bytes]:*package->files) if(name!=stem+argv[5]) kept.push_back({name,&bytes});
            check(kept.size()+1==package->files->size(),"--export-package: the map has no such companion to leave out");
            net::Writer w;w.u32(2);w.str(package->mapPath);w.u32(uint32_t(kept.size()));
            for(const auto& [name,bytes]:kept){w.str(name);w.u32(uint32_t(bytes->size()));w.b.insert(w.b.end(),bytes->begin(),bytes->end());}
            const auto digest=crypto::toHex(crypto::sha256(w.b.data(),w.b.size()));
            check(bool(net::maps::decode(w.b,digest)),"--export-package: the crafted package does not decode");
            write(argv[3],w.b);return 0;
        }
        write(argv[3],package->bytes);return 0;
    }
    if(argc==5 && std::string(argv[1])=="--export-kmp") {
        // A Cartographer-style one-map snapshot of an installed map, for --play-map.
        const auto package=net::maps::build(hpi::mountRetailRoot(argv[2]),argv[4]);
        std::vector<hpi::PackFile> files;
        for(const auto& [path,bytes]:*package->files)files.push_back({path,bytes});
        write(argv[3],hpi::pack(files));return 0;
    }
    if(argc==5 && std::string(argv[1])=="--solo-network")return soloNetwork(uint16_t(std::stoi(argv[2])),argv[3],argv[4]);
    if(argc==6 && std::string(argv[1])=="--room-edit")return roomEdit(uint16_t(std::stoi(argv[2])),argv[3],argv[4],argv[5]);
    if (argc >= 5 && std::string(argv[1]) == "--network") {
        const auto port = uint16_t(std::stoi(argv[2]));
        const bool diplomacy=std::getenv("TAK_DIPLOMACY_NETWORK")!=nullptr;
        if (argc == 5) {
            auto original = hpi::mountRetailRoot(argv[3]);
            auto package = net::maps::build(original, "Ulasem Arena");
            std::vector<hpi::PackFile> entries;
            for (const auto& [name,data] : *package->files) {
                std::string path = name;
                const auto stem=package->mapPath.substr(0,package->mapPath.size()-4);
                if(name.starts_with(stem+"."))path="kmap/transfer test"+name.substr(stem.size());
                if (name == package->mapPath) {
                    auto m=tnt::Map::load(data);
                    m.features[size_t(m.width)*10+10]=uint16_t(m.featureNames.size());
                    m.featureNames.push_back("transfer_tree");
                    entries.push_back({path,m.save()});
                } else entries.push_back({path,data});
            }
            const std::string feature="[transfer_tree]\n{\nblocking=1;\nfootprintx=2;\nfootprintz=2;\nreclaimable=1;\nenergy=75;\n}\n";
            entries.push_back({"features/transfer-test.tdf",{feature.begin(),feature.end()}});
            auto companion=[&](const std::string& suffix,const std::vector<uint8_t>& data) {
                const auto path="kmap/transfer test"+suffix;
                std::erase_if(entries,[&](const auto& entry){return entry.path==path;});
                entries.push_back({path,data});
            };
            crt::Scenario authored;authored.players.resize(2);
            authored.players[1].push_back({{{20,{}}},{{13,{"All Players","Transferred scenario"}}}});
            if(std::getenv("TAK_MAP_TEST_SCENARIO")) {
                auto starts=sim::parseStartPositions(original,package->mapPath);
                check(starts.size()>=2,"scenario transfer fixture needs two start positions");
                for(int p=0;p<2;++p) {
                    crt::Unit unit;unit.objectName=sim::kMonarchs[p];unit.player=p;
                    unit.x=int(starts[size_t(p)].first/16);unit.z=int(starts[size_t(p)].second/16);
                    authored.units.push_back(unit);
                }
                authored.players[1][0].conditions[0].opcode=0;
                for(auto& entry:entries) if(entry.path=="kmap/transfer test.ota") {
                    auto metadata=tnt::Scenario::parse(std::string(entry.data.begin(),entry.data.end()));
                    metadata.hasScenario=true;const auto text=metadata.write()+"\n[TAKPlaytest]{\nauthoredscenario=1;\n}\n";entry.data={text.begin(),text.end()};
                }
            }
            companion(".crt",crt::write(authored));
            companion(".tdf",{});
            const std::string names="TAK_EDITOR_RULE_NAMES 1\n1 0 \"Transfer rule\"\n";
            companion(".editor",{names.begin(),names.end()});
            write(std::filesystem::path(argv[3])/"Maps"/"transfer-test.kmp",hpi::pack(entries));
            if (std::getenv("TAK_MAP_TEST_MISMATCH")) {
                for (auto& entry:entries) if(entry.path.ends_with(".tnt")) {
                    auto m=tnt::Map::load(entry.data); m.heights[100] ^= 1; entry.data=m.save();
                }
                write(std::filesystem::path(argv[4])/"Maps"/"different-copy.kmp",hpi::pack(entries));
            }
        }
        hpi::Vfs hostData = hpi::mountRetailRoot(argv[3]);
        hpi::Vfs peerData = hpi::mountRetailRoot(argv[4]);
        // Each player's downloads and saved generated maps go to its own per-user
        // folder; the data folders are only read. TAK_MAP_TEST_READONLY takes write
        // permission off them (and their Maps/) and checks nothing in them changed.
        const auto cacheBase = std::filesystem::temp_directory_path() / ("tak-map-net-" + crypto::toHex(crypto::randomVec(8)));
        struct CleanCache { std::filesystem::path p; ~CleanCache() { std::error_code ec; std::filesystem::remove_all(p,ec); } } cleanCache{cacheBase};
        const char* spectatorRoot = std::getenv("TAK_MAP_TEST_SPECTATOR_ROOT");
        const bool readOnlyData = std::getenv("TAK_MAP_TEST_READONLY") != nullptr;
        std::vector<std::filesystem::path> dataRoots{argv[3], argv[4]};
        if (spectatorRoot) dataRoots.push_back(spectatorRoot);
        std::vector<decltype(tree({}))> before;
        if (readOnlyData) for (const auto& data : dataRoots) {
            readOnly(data);
            if (std::error_code ec; std::filesystem::is_directory(data/"Maps",ec)) readOnly(data/"Maps");
            before.push_back(tree(data));
        }
        net::MpClient host, peer, spectator;
        host.setMapRoot(argv[3]); peer.setMapRoot(argv[4]); spectator.setMapRoot(spectatorRoot ? spectatorRoot : argv[4]);
        host.setCacheRoot(cacheBase/"host"); peer.setCacheRoot(cacheBase/"peer"); spectator.setCacheRoot(cacheBase/(spectatorRoot ? "spectator" : "peer"));
        host.setDataHash(hpi::gameplayHash(hostData));
        peer.setDataHash(hpi::gameplayHash(peerData)); spectator.setDataHash(peer.dataHash());
        check(host.connect("127.0.0.1", port, "maphost"), "host connection");
        check(peer.connect("127.0.0.1", port, "mappeer"), "peer connection");
        {   // A server refuses a room for a random map this build cannot make (a v9 id).
            net::MpClient refused; refused.setMapRoot(argv[3]); refused.setCacheRoot(cacheBase/"refused");
            refused.setDataHash(host.dataHash());
            check(refused.connect("127.0.0.1", port, "maprefused"), "refused connection");
            bool sent = false;
            for (auto end = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                 refused.poll() && std::chrono::steady_clock::now() < end;) {
                if (!sent && refused.state() == net::MpClient::State::Lobby) {
                    net::GameOptions opts; opts.gameMods = 0;
                    refused.createGame("refused", "", "~gen1~09000100000000000000038001800108ffffffffff0004", opts, 2);
                    sent = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            check(sent && refused.state() == net::MpClient::State::Done &&
                  refused.error().find("generate a new map") != std::string::npos,
                  "a room for an unknown random-map recipe was not refused");
        }
        bool created = false, joined = false, readied = false, started = false, late = false;
        bool chatSent=false,sharingSent=false,sharingSeen=false,sharingRestored=false;
        std::vector<std::pair<std::string,std::string>> hostChat,peerChat,spectatorChat;
        bool hostLoaded = false, peerLoaded = false, spectatorLoaded = false, spectateSent = false;
        uint32_t ht=0, pt=0, st=0;
        std::map<uint32_t,uint64_t> hashes;
        sim::World wh, wp, ws; sim::TypeRegistry reg;
        sim::setupRegistry(reg, peerData, true);
        const std::string mapId = argc > 5 ? argv[5] : "Transfer Test";
        auto setup = [&](net::MpClient& client, hpi::Vfs& base, sim::World& world) {
            auto room = client.startRoom();
            hpi::Vfs view(&base);
            if (client.mapPackage()) view.setMapFiles(client.mapPackage()->files);
            sim::MatchConfig cfg; cfg.vfs = &view;
            cfg.mapPath = client.mapPackage() ? client.mapPackage()->mapPath : hpi::findMap(view, room.mapId);
            cfg.startSeed = client.startSeed(); cfg.startMode = sim::StartMode(net::normalizeStartMode(room.opts.randomStarts));
            cfg.unitCap = room.opts.unitCap;
            cfg.slots.resize(diplomacy ? 3 : 2); cfg.slots[0] = {true,0,0,1,false,false};
            cfg.slots[1] = {true,1,diplomacy ? 0 : 1,1,false,false};
            if(diplomacy)cfg.slots[2]={true,2,1,1,true,true};
            if(argc==5) {
                const auto authored=crt::parse(view.read("kmap/transfer test.crt"));
                check(authored.players.size()==2 && authored.players[1].size()==1 &&
                      authored.players[1][0].actions.size()==1 &&
                      authored.players[1][0].actions[0].slot[1]=="Transferred scenario",
                      "scenario companion missing on a network participant");
                check(view.has("kmap/transfer test.editor") && view.has("kmap/transfer test.tdf") &&
                      view.read("kmap/transfer test.tdf").empty(),"editor metadata or empty restriction lost over network");
            }
            sim::setupMatch(world, reg, cfg);
            if(std::getenv("TAK_MAP_TEST_SCENARIO"))
                check(world.scenario() && world.units().size()==2,"network peer did not initialize authored scenario");
            client.reportLoaded(hpi::gameplayHash(base));
        };
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(90);
        while (std::chrono::steady_clock::now() < until) {
            check(host.poll(), host.error().c_str()); check(peer.poll(), peer.error().c_str());
            if (!created && host.state() == net::MpClient::State::Lobby) {
                net::GameOptions opts; opts.crusades = 1; opts.gameMods = 0;
                host.createGame("map transfer test", "", mapId, opts, diplomacy ? 3 : 2); created = true;
            }
            if (!joined && host.room().id && peer.state() == net::MpClient::State::Lobby) {
                peer.joinGame(host.room().id, ""); joined = true;
            }
            if (!readied && peer.room().id) {
                // START must remain blocked even when human READY arrived first.
                host.setSlot(0,1,0,0,0,1); peer.setSlot(1,1,1,1,diplomacy ? 0 : 1,1);
                if(diplomacy)host.setSlot(2,2,2,2,1,1,0);
                readied = true;
            }
            if (!started && readied && host.room().mapsReady && host.room().slots[1].ready && (!diplomacy || host.room().slots[2].type==2)) {
                host.startGame(); started = true;
            }
            if (!hostLoaded && host.starting()) { setup(host,hostData,wh); hostLoaded = true; }
            if (!peerLoaded && peer.starting()) { setup(peer,peerData,wp); peerLoaded = true; }
            if (hostLoaded && peerLoaded && !late) {
                check(wh.stateHash() == wp.stateHash(), "different initial map worlds");
                check(spectator.connect("127.0.0.1", port, "lateviewer"), "spectator connect"); late = true;
            }
            if (late) {
                check(spectator.poll(), spectator.error().c_str());
                if (!spectateSent && spectator.state() == net::MpClient::State::Lobby) { spectator.spectate(host.room().id, ""); spectateSent = true; }
                if (!spectatorLoaded && (spectator.starting() || spectator.isCatchUp())) {
                    setup(spectator,peerData,ws); spectatorLoaded = true;

                }
            }
            if (diplomacy && hostLoaded && ht>30 && !sharingSent) {
                net::Command c;c.kind=net::Cmd::ShareMana;c.targetId=1;c.queue=0;
                c.player=7; // the server must stamp the actual sender, not trust this
                host.sendCommands({c});sharingSent=true;
            }
            if (diplomacy && sharingSent && !(wh.player(0).manaShareMask & 2)) sharingSeen=true;
            if (diplomacy && sharingSeen && ht>150 && !sharingRestored) {
                net::Command c;c.kind=net::Cmd::ShareMana;c.targetId=1;c.queue=1;
                host.sendCommands({c});sharingRestored=true;
            }
            if (spectatorLoaded && ht>30 && !chatSent) {
                host.chat("PRIVATE_PEER",2);host.chat("NOBODY",0);host.chat("EVERYONE");
                peer.chat("PRIVATE_HOST",1);chatSent=true;
            }
            auto collect=[](net::MpClient& client,auto& messages) {
                auto incoming=client.takeChat();messages.insert(messages.end(),incoming.begin(),incoming.end());
            };
            collect(host,hostChat);collect(peer,peerChat);collect(spectator,spectatorChat);
            auto advance = [&](net::MpClient& client, sim::World& world, uint32_t& tick, bool reference) {
                net::Bundle bundle;
                while (tick < 300 && (reference || hashes.count(tick)) && client.takeBundle(tick,bundle)) {
                    for (const auto& c:bundle.cmds) sim::applyCommand(world,reg,c);
                    for (const auto& e:bundle.events) sim::applyEvent(world,e);
                    world.tick(1.f/30.f);
                    const auto hash=world.stateHash();
                    if (reference) hashes[tick]=hash;
                    else check(hashes.at(tick)==hash,"map worlds diverged in live ticks");
                    if (!client.isSpectator() && tick % net::kHashPeriod == 0) client.sendHash(tick,hash);
                    ++tick;
                }
            };
            if (hostLoaded) advance(host,wh,ht,true);
            if (peerLoaded && ht>pt) advance(peer,wp,pt,false);
            if (spectatorLoaded && ht>st) advance(spectator,ws,st,false);
            check(!host.desynced() && !peer.desynced(), "server map differs");
            if (ht==300 && pt==300 && st==300) {
                if(diplomacy) {
                    check(sharingSeen && sharingRestored && wh.player(0).manaShareMask==0xff &&
                          wp.player(0).manaShareMask==0xff && ws.player(0).manaShareMask==0xff,
                          "sharing command did not apply and restore on all peers");
                    std::cout << "mana sharing: server-stamped sender, disable/enable, referee and spectator lockstep passed\n";
                }
                auto has=[](const auto& messages,const char* text) {
                    return std::any_of(messages.begin(),messages.end(),[&](const auto& item){return item.second==text;});
                };
                check(chatSent && has(hostChat,"PRIVATE_HOST") && has(peerChat,"PRIVATE_PEER"),"directed chat missing recipient");
                check(!has(hostChat,"PRIVATE_PEER") && !has(peerChat,"PRIVATE_HOST") &&
                      !has(spectatorChat,"PRIVATE_PEER") && !has(spectatorChat,"PRIVATE_HOST"),"private chat escaped recipient mask");
                for(const auto* messages:{&hostChat,&peerChat,&spectatorChat}) {
                    check(has(*messages,"EVERYONE"),"default chat did not reach everyone");
                    check(!has(*messages,"NOBODY"),"empty chat recipient mask delivered a message");
                }
                std::cout << "directed chat: recipients, exclusion, empty mask, broadcast and spectator privacy passed\n";
                if (mapgen::isGeneratedMapId(mapId)) {
                    const auto file="Generated-"+crypto::toHex(crypto::sha256(mapId))+".kmp";
                    check(std::filesystem::exists(cacheBase/"host"/"Maps"/file),"host did not save generated map");
                    check(std::filesystem::exists(cacheBase/"peer"/"Maps"/file),"peer did not save generated map");
                } else {
                    check(std::filesystem::exists(cacheBase/"peer"/"MapCache"/(peer.mapPackage()->digest+".takmap")),"peer did not cache the downloaded map");
                }
                for (size_t i = 0; i < before.size(); ++i)
                    check(tree(dataRoots[i]) == before[i], "a data folder was written");
                if (readOnlyData) std::cout << "read-only data folders: unchanged; downloads in the per-user cache\n";
                std::cout << "network map transfer: host, peer, late spectator, 300 matching ticks, hash " << std::hex << wh.stateHash() << '\n';
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error("map transfer timed out: " + host.mapStatus() + "; " + peer.mapStatus());
    }
    const auto root = std::filesystem::temp_directory_path() / ("tak-map-test-" + crypto::toHex(crypto::randomVec(8)));
    struct Clean { std::filesystem::path p; ~Clean() { std::error_code ec; std::filesystem::remove_all(p,ec); } } clean{root};
    tnt::Map map; map.width=map.height=64; map.blocksX=map.blocksY=32; map.seaLevel=0;
    map.heights.assign(4096,40); map.features.assign(4096,0xffff);
    map.tileKeys.assign(1024,0x1234); map.tileCols.assign(1024,0); map.tileRows.assign(1024,0);
    auto files = std::make_shared<hpi::Vfs::Files>();
    (*files)["kmap/test.tnt"] = map.save(); (*files)["terrain/00001234.jpg"] = {1,2,3};
    std::string ota="[GlobalHeader]\n{\ngravity=99;\n}\n";
    (*files)["kmap/test.ota"] = {ota.begin(),ota.end()};
    crt::Scenario scenario;
    scenario.players.resize(2);
    crt::RuleGroup group;
    group.actions.push_back({0, {"transferred scenario", "", "", "", ""}});
    scenario.players[1].push_back(group);
    (*files)["kmap/test.crt"] = crt::write(scenario);
    (*files)["kmap/test.tdf"] = {}; // An empty restriction list must survive too.
    (*files)["kmap/test.txt"] = {'m','a','p'};
    (*files)["kmap/test.editor"] = {'n','a','m','e'};
    (*files)["kmap/test.recipe"] = {'s','e','e','d'};
    // Neither another map's metadata nor arbitrary scripts belong in this package.
    (*files)["kmap/other.crt"] = crt::write(scenario);
    (*files)["scripts/evil.cob"] = {1};
    hpi::Vfs source; source.setMapFiles(files);
    auto p = net::maps::build(source,"test");
    check(p->digest == net::maps::build(source,"TEST")->digest, "unstable map fingerprint");
    {
        // A resource that will ultimately be rejected as unreferenced must not
        // get an unlimited parser budget before the reference check.
        auto hostile=*p->files;
        std::string text;for(int i=0;i<70000;++i)text+="[x]{}\n";
        hostile["features/unreferenced.tdf"]={text.begin(),text.end()};
        net::Writer encoded;encoded.u32(2);encoded.str(p->mapPath);encoded.u32(uint32_t(hostile.size()));
        for(const auto& [name,data]:hostile) {
            encoded.str(name);encoded.u32(uint32_t(data.size()));
            encoded.b.insert(encoded.b.end(),data.begin(),data.end());
        }
        rejects([&]{net::maps::decode(encoded.b,"");},"unreferenced adversarial TDF accepted");
        tak::StopSource cancelled;cancelled.request_stop();
        rejects([&]{net::maps::decode(p->bytes,p->digest,cancelled.get_token());},"cancelled map decoded");
    }

    check(!net::maps::authoredScenario(*p),"ordinary CRT map became a solo scenario");
    // MAP SCENARIO (259): rules the .ota does not declare are leftovers, never offered.
    check(net::maps::scenarioInfo(*p).status==sim::ScenarioStatus::Leftover && !net::maps::scenarioInfo(*p).runnable(),
          "undeclared CRT data became a playable map scenario");
    {
        auto authoredFiles=std::make_shared<hpi::Vfs::Files>(*p->files);
        auto authored=*p;authored.files=authoredFiles;
        auto metadata=tnt::Scenario::parse(ota);metadata.hasScenario=true;
        auto setMetadata=[&](const std::string& text) {
            (*authoredFiles)["kmap/test.ota"]={text.begin(),text.end()};
        };
        setMetadata(metadata.write());
        check(!net::maps::authoredScenario(authored),"hasScenario alone enabled solo skirmish");
        // ... but it is what MAP SCENARIO offers: the Test Map opt-in keeps its own meaning.
        check(net::maps::scenarioInfo(authored).offerable(),"a declared package scenario is not offered");
        setMetadata(metadata.write()+"\n[TAKPlaytest]{\nauthoredscenario=1;\n}\n");
        check(net::maps::authoredScenario(authored),"explicit valid authored scenario cannot start solo");
        hpi::Vfs view;view.setMapFiles(authoredFiles);
        check(net::maps::authoredScenario(view,"kmap/test.tnt"),"local and packaged scenario detection differ");
        {
            const auto packaged=net::maps::scenarioInfo(authored);
            const auto local=sim::inspectScenario(view,"kmap/test.tnt");
            check(packaged.status==local.status && packaged.requiredSeats()==local.requiredSeats() &&
                  packaged.outcomes==local.outcomes && packaged.useOnlyTypes==local.useOnlyTypes,
                  "local and packaged scenario facts differ");
            check(packaged.status==sim::ScenarioStatus::Ok && packaged.marker && !packaged.offerable() &&
                  packaged.active(false) && packaged.requiredSeats()==0b1 && !packaged.outcomes,
                  "packaged Test Map scenario facts");
        }
        const auto crt=authoredFiles->at("kmap/test.crt");
        authoredFiles->erase("kmap/test.crt");
        check(!net::maps::authoredScenario(authored),"missing CRT enabled solo exception");
        (*authoredFiles)["kmap/test.crt"]={1,2,3};
        check(!net::maps::authoredScenario(authored),"damaged CRT enabled solo exception");
        (*authoredFiles)["kmap/test.crt"]=crt;
        metadata.hasScenario=false;
        setMetadata(metadata.write()+"\n[TAKPlaytest]{\nauthoredscenario=1;\n}\n");
        check(!net::maps::authoredScenario(authored),"disabled scenario enabled solo exception");
        setMetadata(metadata.write()+"\n[TAKPlaytest]{\nauthoredscenario=0;\n}\n");
        check(!net::maps::authoredScenario(authored),"disabled opt-in marker enabled solo exception");
    }
    for (const auto* extension : {".ota", ".crt", ".tdf", ".txt", ".editor", ".recipe"}) {
        const auto path = std::string("kmap/test") + extension;
        check(p->files->at(path) == files->at(path), "map companion lost or modified");
        auto changedFiles = std::make_shared<hpi::Vfs::Files>(*files);
        changedFiles->at(path).push_back(' ');
        hpi::Vfs changedSource; changedSource.setMapFiles(changedFiles);
        check(net::maps::build(changedSource,"test")->digest != p->digest,
              "map companion excluded from fingerprint");
    }
    check(!p->files->count("kmap/other.crt") && !p->files->count("scripts/evil.cob"),
          "unrelated files included in package");
    rejects([&]{net::maps::saveCache(root,*p,1);},"disk quota accepted oversized package");
    check(!std::filesystem::exists(root/"MapCache"/(p->digest+".takmap")),"quota rejection left partial cache");
    net::maps::saveCache(root,*p);
    net::maps::saveCache(root,*p,1); // an existing download needs no additional quota

    rejects([&]{net::maps::loadCache(root,p->digest,p->bytes.size()-1);},"actual cache size escaped budget");
    rejects([&]{net::maps::loadCache(root,p->digest,net::maps::kMaxBytes,p->bytes.size()-1);},"cache advertised mismatch accepted");
    check(std::filesystem::exists(root/"MapCache"/(p->digest+".takmap")),"admission removed valid cache");
    const auto cached = net::maps::loadCache(root,p->digest);
    check(cached && *cached->files == *p->files, "cache companion roundtrip");
    hpi::Vfs catalog; catalog.refreshMapCache(root);
    const auto listed=hpi::listMaps(catalog);
    check(listed.size()==1,"download absent from subsequent map picker");
    check(!catalog.has("terrain/00001234.jpg"),"cached map art leaked globally");
    check(net::maps::build(catalog,listed.front().first)->digest==p->digest,"reselected download changed identity");
    {
        const auto snapshotPath=root/"Test Snapshot.kmp";
        auto changed=std::make_shared<hpi::Vfs::Files>(*p->files);
        changed->at("kmap/test.txt")={'e','d','i','t','e','d'};
        std::vector<hpi::PackFile> members;
        for(const auto& [name,data]:*changed)members.push_back({name,data});
        auto writeSnapshot=[&] {
            const auto bytes=hpi::pack(members);
            std::ofstream output(snapshotPath,std::ios::binary|std::ios::trunc);
            output.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
        };
        writeSnapshot();
        const auto imported=net::maps::importSnapshot(catalog,snapshotPath);
        check(imported->files->at("kmap/test.txt")==changed->at("kmap/test.txt"),"older cached map shadowed current playtest snapshot");
        check(imported->digest!=p->digest,"edited snapshot has stale fingerprint");
        members.push_back({"kmap/second.tnt",map.save()});writeSnapshot();
        rejects([&]{net::maps::importSnapshot(catalog,snapshotPath);},"ambiguous multi-map playtest bundle accepted");
    }
    auto corrupt = p->bytes; corrupt.back() ^= 1;
    rejects([&]{net::maps::decode(corrupt,p->digest);}, "corruption accepted");
    auto bad = std::make_shared<hpi::Vfs::Files>(*files);
    (*bad)["kmap/test.ota"].push_back(' '); hpi::Vfs changed; changed.setMapFiles(bad);
    check(net::maps::build(changed,"test")->digest != p->digest, "scenario mismatch ignored");
    (*bad)["terrain/00001234.jpg"].push_back(4);
    check(net::maps::build(changed,"test")->digest != p->digest, "art mismatch ignored");
    hpi::Vfs room(&source); room.setMapFiles(bad);
    check(source.read("terrain/00001234.jpg").size()==3 && room.read("terrain/00001234.jpg").size()==4,"room data leaked");
    net::maps::Receiver receive; receive.begin(1,7,uint32_t(p->bytes.size()),p->digest);
    for (size_t off=0; off<p->bytes.size();) {
        net::Writer w; w.u32(1); w.u32(7); w.u32(uint32_t(off));
        size_t n=std::min(net::maps::kChunkBytes,p->bytes.size()-off);
        w.b.insert(w.b.end(),p->bytes.begin()+off,p->bytes.begin()+off+n);
        net::Reader r(w.b.data(),w.b.size()); off+=n;
        check(receive.append(r)==(off==p->bytes.size()?net::maps::Append::Done:net::maps::Append::More),"chunk progress");
    }
    check(receive.complete() && receive.bytes==p->bytes,"chunk assembly");
    rejects([&]{receive.begin(1,7,uint32_t(net::maps::kMaxBytes+1),p->digest);},"oversized offer");
    receive.begin(1,7,uint32_t(p->bytes.size()),p->digest);
    // A chunk of another room, or of an earlier request (xfer) of this one, is left over
    // from a transfer that is over: dropped quietly, never a MAP ERROR.
    auto chunk=[](uint32_t room,uint32_t xfer,uint32_t offset){net::Writer w;w.u32(room);w.u32(xfer);w.u32(offset);w.u8(1);return w;};
    auto append=[&](const net::Writer& w){net::Reader r(w.b.data(),w.b.size());return receive.append(r);};
    check(append(chunk(2,7,0))==net::maps::Append::Stale && receive.bytes.empty(),"another room's chunk is stale");
    check(append(chunk(1,6,0))==net::maps::Append::Stale && receive.bytes.empty(),"an earlier transfer's chunk is stale");
    rejects([&]{append(chunk(1,7,1));},"out-of-order chunk");
    {
        net::maps::Receiver idle;net::Writer w=chunk(1,7,0);net::Reader r(w.b.data(),w.b.size());
        check(idle.append(r)==net::maps::Append::Stale,"a chunk with no transfer under way is stale");
    }
    net::Writer hostile; hostile.u32(2); hostile.str("kmap/../evil.tnt"); hostile.u32(1);
    rejects([&]{net::maps::decode(hostile.b,"");},"traversal accepted");
    hostile={}; hostile.u32(2); hostile.str("kmap/test.tnt"); hostile.u32(2); hostile.str("scripts/evil.cob"); hostile.u32(1); hostile.u8(0);
    rejects([&]{net::maps::decode(hostile.b,"");},"script import accepted");
    auto rawPackage = [&](const std::string& extra, const std::vector<uint8_t>& payload) {
        net::Writer w; w.u32(2); w.str(p->mapPath); w.u32(uint32_t(p->files->size()+1));
        for (const auto& [name, data] : *p->files) {
            w.str(name); w.u32(uint32_t(data.size())); w.b.insert(w.b.end(),data.begin(),data.end());
        }
        w.str(extra); w.u32(uint32_t(payload.size())); w.b.insert(w.b.end(),payload.begin(),payload.end());
        return w.b;
    };
    for (const auto* extra : {"kmap/other.crt", "kmap/test.cob", "kmap/test/evil.crt",
                             "kmap/test.crt/../evil.crt", "kmap/test.editor", "units/test.tdf"})
        rejects([&]{net::maps::decode(rawPackage(extra,{1}),"");},"unrelated or duplicate companion accepted");
    auto legacyFiles = std::make_shared<hpi::Vfs::Files>();
    (*legacyFiles)["kmap/test.tnt"] = map.save();
    (*legacyFiles)["terrain/00001234.jpg"] = {1,2,3};
    hpi::Vfs legacy; legacy.setMapFiles(legacyFiles);
    check(net::maps::build(legacy,"test")->files->size()==2,"map without companions rejected");
    {   // A saved random map is an ordinary map: its .recipe companion is only carried,
        // so one from another generator version still plays from its saved terrain.
        auto savedFiles = std::make_shared<hpi::Vfs::Files>(*legacyFiles);
        const std::string oldRecipe = "~gen1~09000100000000000000038001800108ffffffffff0004";
        (*savedFiles)["kmap/test.recipe"] = std::vector<uint8_t>(oldRecipe.begin(), oldRecipe.end());
        hpi::Vfs saved; saved.setMapFiles(savedFiles);
        const auto package = net::maps::decode(net::maps::build(saved,"test")->bytes,"");
        check(package->mapPath=="kmap/test.tnt" && package->files->count("kmap/test.recipe"),
              "a saved random map of another generator version is not playable as an ordinary map");
    }
    clientRoomEdits(root/"client-edit", *p);
    if (argc == 2) {
        auto vfs=hpi::mountRetailRoot(argv[1]);
        {
            // MAP SCENARIO (259) on the shipped maps, read from the very packages a room
            // verifies: what the CHANGE MAP panel offers and the seats the room requires.
            using S=sim::ScenarioStatus;
            sim::TypeRegistry registry;sim::setupRegistry(registry,vfs,false);
            auto info=[&](const char* name){return net::maps::scenarioInfo(*net::maps::build(vfs,name),&registry);};
            const auto koth=info("King of the Hill");
            check(koth.status==S::Ok && koth.requiredSeats()==0x0F && koth.useOnlyTypes==6 && koth.outcomes,
                  "King of the Hill: offered, seats 1-4, six Use Only types, victory rules");
            check(!koth.useOnlyMissing && (koth.armySeats&0x0F)==0x0F,
                  "King of the Hill: its package carries the Use Only list, and every seat gets an army");
            const auto strength=info("Increasing Strength");
            check(strength.status==S::Ok && strength.placed==0 && strength.requiredSeats()==0x0F,
                  "Increasing Strength: offered, rules only, seats 1-4");
            const auto savannah=info("Savannah Hunt");
            check(savannah.status==S::Ok && savannah.requiredSeats()==0xFF,"Savannah Hunt: offered, all eight seats");
            check(info("Iapur Narrows").status==S::Leftover,"Iapur Narrows: retail's unused rules are a leftover");
            check(info("Angvir's Maze").status==S::Leftover,"Angvir's Maze: retail's unused rules are a leftover");
            check(info("Crab Cove").status==S::Empty,"Crab Cove: an empty scenario");
            check(info("Inner Circle").status==S::NoScenario,"Inner Circle: no scenario");
            for (const char* name:{"Cairbray Coast Landing","Frey River Plain","Ground War","Increasing Strength",
                                   "King of the Hill","Riparian Conflict","Savannah Hunt","Temple of Blood",
                                   "threesacharm","Ulin's Folly","Varro Passage"})
                check(info(name).offerable(),(std::string(name)+": shipped scenario not offered").c_str());
        }
        mapgen::Params params; params.waterDensity=0; params.name="My Lake";
        auto recipe=mapgen::encodeMapId(params);
        auto output=net::maps::saveGenerated(root,vfs,recipe);
        check(output.filename().string().starts_with("Generated-My Lake-"),"named map filename");
        check(hpi::Archive(output).find("kmap/My Lake-"+crypto::toHex(crypto::sha256(recipe)).substr(0,16)+".tnt")!=nullptr,
              "named map archive path");
        const auto savedTime=std::filesystem::last_write_time(output);
        check(net::maps::saveGenerated(root,vfs,recipe)==output && std::filesystem::last_write_time(output)==savedTime,
              "saving a generated map again rewrote it");
        // Only a current recipe is saved: one of another version (here a v9 id) or a
        // malformed one is refused, never generated as some other map.
        for (const char* other:{"~gen1~09000100000000000000038001800108ffffffffff0004","~gen1~0a00zz"}) {
            bool refused=false;
            try { net::maps::saveGenerated(root,vfs,other); }
            catch (const std::exception& e) { refused=std::string(e.what()).find("generate a new map")!=std::string::npos; }
            check(refused,"an unknown random-map recipe was saved");
            check(!std::filesystem::exists(root/"Maps"/("Generated-"+crypto::toHex(crypto::sha256(std::string(other)))+".kmp")),
                  "an unknown random-map recipe left a saved map");
        }
        hpi::Vfs saved(&vfs); saved.addLayer(hpi::MountSet(root/"Maps",{false,{".kmp"},{},{}}));
        const auto maps=hpi::listMaps(saved);
        std::string path;
        for (const auto& [name,pth]:maps) if(name=="My Lake-"+crypto::toHex(crypto::sha256(recipe)).substr(0,16)) path=pth;
        check(!path.empty(),"saved generated map absent from browser");
        auto generated=mapgen::generate(params,vfs);
        auto loaded=tnt::Map::load(saved.read(path));
        // The save adds the radar pictures the generator leaves out; the rest is the map as rolled.
        const auto geometry=[](tnt::Map m) {m.minimap.clear();m.overview.clear();m.minimapW=m.minimapH=m.overviewW=m.overviewH=0;return m.save();};
        check(loaded.minimapW==126 && loaded.minimap.size()==126u*126u && loaded.overviewW>0 &&
              loaded.overview.size()==size_t(loaded.overviewW)*size_t(loaded.overviewH),"saved generated map has no radar pictures");
        check(geometry(loaded)==generated.map.save(),"saved generated geometry differs");
        {
            const auto ota=saved.read(vpath::replaceExtension(path,".ota"));
            const auto size="size="+std::to_string(generated.map.width/32)+" x "+std::to_string(generated.map.height/32)+";";
            check(std::string(ota.begin(),ota.end()).find(size)!=std::string::npos,"saved generated map has no size");
        }
        auto starts=sim::parseStartPositions(saved,path);
        check(starts.size()==generated.starts.size(),"saved start count");
        for(size_t i=0;i<starts.size();++i) check(starts[i].first==generated.starts[i].first*16 && starts[i].second==generated.starts[i].second*16,"saved start location");
        const auto time=std::filesystem::last_write_time(output);
        check(net::maps::saveGenerated(root,vfs,recipe)==output && std::filesystem::last_write_time(output)==time,"duplicate generated save");
        // Same title, different recipe: neither VFS path nor browser selection
        // may mask the other map (including case-insensitive title collisions).
        for (int i=0;i<2;++i) {
            auto other=params; other.seed+=i+1;
            if (i) other.name="my lake";
            const auto otherRecipe=mapgen::encodeMapId(other);
            const auto otherOutput=net::maps::saveGenerated(root,vfs,otherRecipe);
            check(otherOutput!=output,"duplicate title overwrote archive");
            hpi::Vfs both(&vfs); both.addLayer(hpi::MountSet(root/"Maps",{false,{".kmp"},{},{}}));
            const auto entries=hpi::listMaps(both);
            const auto otherName=other.name+"-"+crypto::toHex(crypto::sha256(otherRecipe)).substr(0,16);
            int found=0;
            for (const auto& [title,pth]:entries) {
                if (pth==path) {
                    ++found;
                    check(geometry(tnt::Map::load(both.read(pth)))==generated.map.save(),"original duplicate title changed geometry");
                }
                if (title==otherName) {
                    ++found;
                    check(hpi::findMap(both,title)==pth,"duplicate title cannot be selected");
                    check(geometry(tnt::Map::load(both.read(pth)))==mapgen::generate(other,vfs).map.save(),"duplicate title loaded wrong geometry");
                    check(net::maps::build(both,title)->mapPath==hpi::MountSet::key(pth),"duplicate title package resolved wrong map");
                }
            }
            check(found==2,"duplicate titles not independently listed");
        }

    }
    std::cout << "map transfer: package identity, isolation, cache, chunks, and validation passed\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<'\n'; return 1;}
