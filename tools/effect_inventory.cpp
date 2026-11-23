// Audit exact sequence requests from weapons and common script effects.
// Includes explosion definitions; pixel equivalence remains outside this inventory.
#include "hpi/hpi.h"
#include "gaf/gaf.h"
#include "gaf/nimbus.h"
#include "sim/matchsetup.h"
#include "tdo/tdo.h"
#include <iostream>
#include <set>

int main(int argc, char** argv) {
    const bool weaponAudit=argc==3 && std::string(argv[2])=="--weapons";
    if (argc != 2 && !weaponAudit) return 2;
    const auto vfs = tak::hpi::mountRetailRoot(argv[1]);
    if(weaponAudit) {
        size_t moving=0,artless=0,missingModels=0,unreadableModels=0,emptyModels=0;
        for(bool crusades:{false,true}) {
            tak::sim::TypeRegistry registry;
            tak::sim::setupRegistry(registry,vfs,crusades);
            for(const auto& [id,type]:registry.types())for(const auto& weapon:type.weapons) {
                const bool renderedShot=!weapon.melee && weapon.kind==tak::sim::Weapon::Kind::Normal &&
                    weapon.flameKind<0 && !weapon.lightning &&
                    (weapon.projVel>0.0f || weapon.beam);
                if(!renderedShot)continue;
                ++moving;
                if(!weapon.weaponArt.empty() || !weapon.shotArt.empty() ||
                   weapon.spinRate!=0 || weapon.shotSpin[0]!=0 || weapon.shotSpin[2]!=0)
                    std::cout << "WEAPON_ART " << (crusades?"Crusades":"standard") << ' '
                              << id << " / " << weapon.name << " art=" << weapon.weaponArt
                              << " shotart=" << weapon.shotArt << " model=" << weapon.shotModel
                              << " straight=" << weapon.straight << " beam=" << weapon.beam
                              << " ballistic=" << weapon.ballistic << " spin=" << weapon.spinRate << '\n';
                const bool noModel=weapon.shotModel.empty();
                const bool noSprite=weapon.weaponArt.empty();
                const bool noShotArt=weapon.shotArt.empty();
                if(noModel && noSprite && noShotArt) {
                    ++artless;
                    std::cout << "ARTLESS " << (crusades?"Crusades":"standard") << ' '
                              << id << " / " << weapon.name << " vel=" << weapon.projVel
                              << " beam=" << weapon.beam << '\n';
                }
                if(!weapon.shotModel.empty()) {
                    const auto path="objects3d/"+weapon.shotModel+".3do";
                    const auto bytes=vfs.tryRead(path);
                    if(!bytes) {
                        ++missingModels;
                        std::cout << "MISSING_MODEL " << (crusades?"Crusades":"standard") << ' '
                                  << id << " / " << weapon.name << " model=" << weapon.shotModel << '\n';
                    } else {
                        try {
                            const auto model=tak::tdo::load(*bytes);
                            size_t primitives=0,vertices=0;
                            auto countGeometry=[&](auto&& self,const tak::tdo::Object& object)->void {
                                primitives+=object.primitives.size();
                                vertices+=object.vertices.size()/3;
                                for(const auto& child:object.children)self(self,child);
                            };
                            countGeometry(countGeometry,model.root);
                            if(primitives==0 || vertices==0) {
                                ++emptyModels;
                                std::cout << "EMPTY_MODEL " << (crusades?"Crusades":"standard") << ' '
                                          << id << " / " << weapon.name << " model=" << weapon.shotModel
                                          << " vertices=" << vertices << " polygons=" << primitives << '\n';
                            }
                        } catch(const std::exception& error) {
                            ++unreadableModels;
                            std::cout << "UNREADABLE_MODEL " << (crusades?"Crusades":"standard") << ' '
                                      << id << " / " << weapon.name << " model=" << weapon.shotModel
                                      << " error=" << error.what() << '\n';
                        }
                    }
                }
                if(!weapon.weaponArt.empty() && weapon.weaponArt.find(':') == std::string::npos &&
                   !vfs.has("anims/"+weapon.weaponArt+"_4444.taf") &&
                   !vfs.has("anims/"+weapon.weaponArt+"_1555.taf") &&
                   !vfs.has("anims/"+weapon.weaponArt+".gaf")) {
                    std::cout << "MISSING_SPRITE_BANK " << (crusades?"Crusades":"standard") << ' '
                              << id << " / " << weapon.name << " art=" << weapon.weaponArt << '\n';
                }
            }
        }
        std::cout << "Moving ordinary-projectile slots=" << moving << ", no authored model/sprite="
                  << artless << ", missing model files=" << missingModels
                  << ", unreadable models=" << unreadableModels << ", empty models=" << emptyModels << '\n';
        return missingModels || unreadableModels || emptyModels ? 1 : 0;
    }
    for(const auto& path:vfs.list("gamedata/explosions"))
        if(path.ends_with(".tdf"))std::cout << "Explosion definition: " << path << "\n";
    std::set<std::string> names = {
        "flames:flame large", "flames:flame medium", "flames:flame small",
        "smoke:smoke01", "bigsmoke", "steam", "aramonbuild", "zhonbuild", "tarosbuild",
        "verunabuild", "creonbuild", "mindspin", "transportfx:transswirl", "deathmagic:purpledeath", "pillaroflight"
    };
    for (bool crusades : {false, true}) {
        tak::sim::TypeRegistry registry;
        tak::sim::setupRegistry(registry, vfs, crusades);
        for (const auto& [name, type] : registry.types()) {
            for (const auto& weapon : type.weapons) {
                for (const auto& art : {weapon.weaponArt, weapon.shotArt,
                        weapon.wanderStart, weapon.wanderLoop, weapon.wanderEnd,
                        weapon.radiusArt[0], weapon.radiusArt[1], weapon.radiusArt[2]})
                    if (!art.empty()) names.insert(art);
                if (!weapon.shadowArt.empty()) names.insert("shadows:" + weapon.shadowArt);
            }
        }
    }
    for (const auto& [alias, name] : tak::gaf::factionNimbus(vfs))
        if (!name.empty()) names.insert(name);

    const auto explosionBytes=vfs.read("gamedata/explosions/explosions.tdf");
    const auto explosions=tak::tdf::parseText(std::string(explosionBytes.begin(),explosionBytes.end()));
    for(size_t i=0;i<explosions.childOrder.size();++i)
        std::cout << "Explosion class " << i << ": " << explosions.childOrder[i] << "\n";
    size_t variants=0,differentBanks=0;
    for(const auto& [className,node]:explosions.orderedChildren())
        for(const auto& [variantName,variant]:node->orderedChildren()) {
            const auto bank=variant->valueOr("gaf","");
            const auto animation=variant->valueOr("anim",bank);
            if(animation.empty())continue;
            ++variants;
            if(!bank.empty() && tak::hpi::MountSet::key(bank)!=tak::hpi::MountSet::key(animation)) {
                ++differentBanks;
                std::cout << "Explosion " << className << "/" << variantName << ": " << bank << ":" << animation << "\n";
            }
            names.insert(bank.empty() ? animation : bank+":"+animation);
        }
    std::cout << "Explosion inventory: " << variants << " variants, " << differentBanks << " distinct bank/sequence names\n";

    size_t found = 0, alphaFrames = 0, additiveFrames = 0;
    std::map<unsigned,size_t> encodingFrames;
    size_t compositeFrames=0, mixedCompositeFrames=0;
    const auto compositeInventory=[&](const std::vector<uint8_t>& bytes,const std::string& wanted) {
        const auto word=[&](size_t offset,unsigned count) {
            uint32_t value=0;
            for(unsigned i=0;i<count;++i)value|=uint32_t(bytes.at(offset+i))<<(8*i);
            return value;
        };
        for(uint32_t entry=0;entry<word(4,4);++entry) {
            const size_t base=word(12+entry*4,4);
            std::string name;
            for(unsigned i=0;i<32 && bytes.at(base+8+i);++i)name+=char(bytes.at(base+8+i));
            if(tak::hpi::MountSet::key(name)!=wanted)continue;
            size_t composites=0,mixed=0;
            for(uint32_t frame=0;frame<word(base,2);++frame) {
                const size_t header=word(base+40+frame*8,4);
                const auto children=bytes.at(header+10);
                if(!children)continue;
                ++composites;
                std::set<std::pair<uint8_t,uint8_t>> modes;
                const size_t pointers=word(header+16,4);
                for(unsigned child=0;child<children;++child) {
                    const size_t part=word(pointers+child*4,4);
                    modes.emplace(bytes.at(part+9),bytes.at(part+11));
                }
                mixed+=modes.size()>1;
            }
            compositeFrames+=composites;mixedCompositeFrames+=mixed;
            if(composites)std::cout << wanted << ": " << composites << " composites, "
                                    << mixed << " with mixed child format/flags\n";
            return;
        }
    };
    for (const auto& name : names) {
        const auto colon = name.find(':');
        const auto file = name.substr(0, colon);
        const auto wanted = tak::hpi::MountSet::key(
            colon == std::string::npos ? name : name.substr(colon + 1));
        bool match = false;
        std::string available;
        for (const char* suffix : {"_4444.taf", "_1555.taf", ".taf", ".gaf"}) {
            const auto path = "anims/" + file + suffix;
            if (const auto bytes = vfs.tryRead(path)) {
                try {
                    for (const auto& sequence : tak::gaf::load(*bytes, {}, -1, path)) {
                        available += sequence.name + ",";
                        if (tak::hpi::MountSet::key(sequence.name) == wanted) {
                            match = !sequence.frames.empty();
                            compositeInventory(*bytes,wanted);
                            size_t alpha=0, indexed=0;
                            for(const auto& frame:sequence.frames) {
                                ++encodingFrames[frame.encoding];
                                if(frame.encoding!=4) {++indexed;continue;}
                                if(frame.blendFlag==255) {++alpha;++alphaFrames;}
                                else ++additiveFrames;
                            }
                            if(indexed)std::cout << name << ": " << indexed << " non-4444 frames\n";
                            if(alpha)std::cout << name << ": " << alpha << " native alpha-blended frames\n";

                            if(name=="mindspin" || name=="transportfx:transswirl") {
                                uint32_t ticks=0;
                                for(const auto& frame:sequence.frames)
                                    ticks+=std::max(1u,unsigned(frame.retailDelayTicks));
                                std::cout << name << ": " << sequence.frames.size()
                                          << " frames, " << ticks << " ticks\n";
                            }
                            break;
                        }
                    }
                } catch (const std::exception& error) {
                    std::cerr << path << ": " << error.what() << '\n';
                }
            }
            if (match) break;
        }
        if (match) ++found;
        else std::cout << "MISSING " << name << " available=" << available << '\n';
    }
    std::cout << "Resolved " << found << " / " << names.size() << " unique requests\n";
    std::cout << "4444 blend inventory: " << alphaFrames << " alpha, " << additiveFrames << " additive frames\n";
    for(const auto& [encoding,count]:encodingFrames)
        std::cout << "Encoding " << encoding << ": " << count << " frames\n";
    std::cout << "Composite inventory: " << compositeFrames << " frames, " << mixedCompositeFrames << " mixed\n";
    return found == names.size() ? 0 : 1;
}
