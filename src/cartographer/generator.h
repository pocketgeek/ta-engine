#pragma once
#include "tnt/mapgen.h"
#include <array>
namespace ta::hpi { class Vfs; }
namespace cart {
// Seed, players and hills (the TA generator's relief density).
using GeneratorFields=std::array<std::string,3>;
GeneratorFields generatorFields(const ta::mapgen::Params& params);
bool parseGeneratorFields(const GeneratorFields& fields,ta::mapgen::Params& params,std::string& error);
// The settings of a saved recipe (the .ota's tageneratorrecipe), refused unless it
// encodes back to exactly the same text (never silently use defaults or other inputs).
bool restoreGeneratorRecipe(const std::string& recipe,ta::mapgen::Params& params,std::string& error);
std::string mapGeneratorRecipe(const ta::hpi::Vfs& vfs,const std::string& mapPath,
                               const std::string& embedded);
}
