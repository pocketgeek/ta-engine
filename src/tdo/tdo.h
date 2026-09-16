#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ta::tdo {

// 3DO model format (shared with classic TA, version 1). A model is a tree
// of objects, each with vertices (s32 fixed-point, 1/65536 units), textured
// or colored polygons, and an offset from its parent. Unit animation moves
// these objects via COB scripts.

struct Primitive {
    uint32_t colorIndex = 0;
    std::string texture;             // empty = colored primitive
    std::vector<uint16_t> indices;   // polygon vertex indices
};

struct Object {
    std::string name;
    float x = 0, y = 0, z = 0;       // offset from parent, in world units
    std::vector<float> vertices;     // x,y,z triples, world units
    std::vector<Primitive> primitives;
    std::vector<Object> children;
};

struct Model {
    Object root;
    // All distinct texture names used by the model.
    std::vector<std::string> textures() const;
};

Model load(const std::filesystem::path& file);
// Parse from an in-memory buffer (a VFS-resolved archive entry).
Model load(const std::vector<uint8_t>& d);

} // namespace ta::tdo
