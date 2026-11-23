#include "gaf/nimbus.h"
#include "gaf/animationtiming.h"
#include "gaf/featureburntiming.h"
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>

int main() {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("tak-nimbus-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    struct Cleanup { fs::path path; ~Cleanup() { std::error_code e; fs::remove_all(path,e); } } cleanup{root};
    fs::create_directories(root / "gamedata");fs::create_directories(root / "anims");
    std::ofstream(root / "gamedata/sidedata.tdf") <<
        "[SIDE0]\n{\nname=ZHON;\nnameprefix=ZON;\nnimbus=nimbus_test;\n}\n";
    auto mount = [&] { tak::hpi::Vfs vfs;vfs.addLayer(tak::hpi::MountSet(root));return vfs; };
    auto require = [](bool ok, const char* message) { if(!ok)throw std::runtime_error(message); };
    auto missing = mount();
    const auto missingHash = tak::hpi::gameplayHash(missing);
    require(tak::gaf::factionNimbus(missing).at("zon").empty(),"missing art disables nimbus");
    std::vector<uint8_t> art(89);
    auto word = [&](size_t offset, uint32_t value, int bytes) {
        for(int i=0;i<bytes;++i)art[offset+size_t(i)]=uint8_t(value>>(i*8));
    };
    word(0,0x10100,4);word(4,1,4);word(12,16,4);
    word(16,1,2);word(18,1,2);
    std::memcpy(art.data()+24,"nimbus_test",11);
    word(56,64,4);word(60,3,4);word(64,1,2);word(66,1,2);word(80,88,4);
    auto write = [&] {
        std::ofstream out(root / "anims/nimbus_test.gaf",std::ios::binary);
        out.write(reinterpret_cast<const char*>(art.data()),std::streamsize(art.size()));
    };
    write();auto present=mount();
    auto names=tak::gaf::factionNimbus(present);
    require(names.at("zon")=="nimbus_test" && names.at("zhon")=="nimbus_test","both faction aliases resolve art");
    const auto presentHash=tak::hpi::gameplayHash(present);
    require(presentHash!=missingHash,"missing art changes gameplay checksum");
    art.back()=200;write();
    require(tak::hpi::gameplayHash(mount())==presentHash,"valid pixel recolor remains cosmetic");
    art[1]=0;write();
    require(tak::hpi::gameplayHash(mount())==missingHash,"invalid art is unavailable consistently");
    art[1]=1;write();fs::create_directories(root / "units");
    std::ofstream(root / "units/test.fbi") << "[WEAPON1]\n{\nwanderstartart=nimbus_test;\n}\n";
    const auto stormHash=tak::hpi::gameplayHash(mount());
    word(60,501,4);write();
    require(tak::gaf::animationTiming(mount(),"nimbus_test")==std::vector<uint16_t>{501},
            "simulation retains authored frame duration without cosmetic clamping");
    require(tak::hpi::gameplayHash(mount())!=stormHash,"storm timing affects gameplay checksum");
    const auto changedTimingHash=tak::hpi::gameplayHash(mount());
    art.back()=17;write();
    require(tak::hpi::gameplayHash(mount())==changedTimingHash,"storm pixel recolor remains cosmetic");
    word(64,0,2);write();
    const auto blank=tak::gaf::load(art,tak::gaf::Palette{},-1,"blank-frame fixture");
    require(blank.size()==1 && blank[0].frames.size()==1 && blank[0].frames[0].width==0,
            "blank authored frames preserve sequence indices");
    require(tak::gaf::animationTiming(mount(),"nimbus_test")==std::vector<uint16_t>{501},
            "blank frames retain their authored duration");
    require(tak::gaf::factionNimbus(mount()).at("zon")=="nimbus_test",
            "blank frames do not make an existing animation unavailable");
    // Feature replacement uses the longest overlay, or the body without overlays.
    fs::remove(root / "units/test.fbi");
    word(64,1,2);word(60,3,4);write();
    auto writeEffect = [&](const char* name,uint32_t delay,uint8_t pixel) {
        auto bytes=art;
        if(bytes.size()<64)throw std::runtime_error("effect fixture too short");
        std::memset(bytes.data()+24,0,32);std::memcpy(bytes.data()+24,name,std::strlen(name));
        for(int i=0;i<4;++i)bytes[60+size_t(i)]=uint8_t(delay>>(8*i));
        bytes.back()=pixel;
        std::ofstream out(root / (std::string("anims/")+name+".gaf"),std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));
    };
    writeEffect("frontfx",5,1);writeEffect("backfx",9,1);
    fs::create_directories(root / "features");
    const std::string definition="[tree]\n{\nfilename=nimbus_test;\nseqnameburn=nimbus_test;\n"
        "seqnamefrontflame=frontfx;\nseqnamebackflame=backfx;\n}\n";
    std::ofstream(root / "features/tree.tdf") << definition;
    auto featureDuration=[&](const tak::tdf::Node& node) {
        auto vfs=mount();return tak::gaf::FeatureBurnTiming(vfs).duration(node);
    };
    auto tree=tak::tdf::parseText(definition).children.at("tree");
    require(featureDuration(tree)==9,"burn lifetime waits for the longer overlay");
    const auto featureHash=tak::hpi::gameplayHash(mount());
    writeEffect("backfx",9,17);
    require(tak::hpi::gameplayHash(mount())==featureHash,"feature flame pixel recolor remains cosmetic");
    writeEffect("frontfx",6,1);
    require(tak::hpi::gameplayHash(mount())==featureHash,"shorter overlay change preserves gameplay lifetime");
    writeEffect("backfx",10,1);
    require(tak::hpi::gameplayHash(mount())!=featureHash,"feature retirement timing changes gameplay hash");
    tree.values.erase("seqnamefrontflame");tree.values.erase("seqnamebackflame");
    require(featureDuration(tree)==3,"overlay-free burn uses body lifetime");
    tree.values["seqnameburn"]="missing";
    require(featureDuration(tree)==0,"missing burn body cannot ignite");
    // Byte 11 is blending metadata, not the high half of a subframe count.
    word(64,1,2);word(66,1,2);art.resize(90);word(88,0xffff,2);art[73]=4;
    for(unsigned flag:{0u,1u,128u,254u,255u}) {
        art[75]=uint8_t(flag);art[18]=uint8_t(flag);
        const auto decoded=tak::gaf::load(art,{},-1,"blend-flag fixture");
        require(decoded.at(0).loopFlag==flag,"sequence loop byte survives decoding unchanged");
        const auto& frame=decoded.at(0).frames.at(0);
        require(frame.encoding==4 && frame.blendFlag==flag &&
                frame.rgba==std::vector<uint8_t>({255,255,255,255}),
                "single-frame decoding preserves format and independent blend flag");
    }
    // A flagged composite still has one child, rather than becoming a raw frame.
    art.resize(122);art[74]=1;art[75]=255;word(80,88,4);word(88,94,4);
    word(94,1,2);word(96,1,2);art[103]=4;art[105]=255;
    word(110,120,4);word(120,0xffff,2);
    const auto composite=tak::gaf::load(art,{},-1,"flagged composite fixture");
    require(composite[0].frames[0].blendFlag==255 &&
            composite[0].frames[0].rgba==std::vector<uint8_t>({255,255,255,255}),
            "blend flag does not change composite child count");
    std::cout << "PASS: frame blend flags remain independent of subframe counts\n";
    std::cout << "PASS: nimbus availability, faction aliases and cosmetic-safe gameplay checksum\n";
}
