#pragma once
#include "tnt/mapgen.h"
#include <array>
namespace tak::hpi { class Vfs; }
namespace cart {
// Seed, players, layout, tree/rock/mana/water/relief densities, scattered mana, and
// the v10 plateau/mountain/town amounts.
using GeneratorFields=std::array<std::string,12>;
GeneratorFields generatorFields(const tak::mapgen::Params& params);
bool parseGeneratorFields(const GeneratorFields& fields,tak::mapgen::Params& params,std::string& error);
// The settings of a saved recipe (the .ota's takgeneratorrecipe), refused unless it
// encodes back to exactly the same text (never silently use defaults or other inputs).
bool restoreGeneratorRecipe(const std::string& recipe,tak::mapgen::Params& params,std::string& error);
std::string mapGeneratorRecipe(const tak::hpi::Vfs& vfs,const std::string& mapPath,
                               const std::string& embedded);
}
