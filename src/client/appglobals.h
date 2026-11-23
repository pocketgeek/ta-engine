#pragma once

// File-scope globals shared between main.cpp and GameView (client/gameview.h).
// Kept here (as C++17 inline variables -> one definition across TUs) so both the
// app shell and the extracted GameView translation units see the same instance.

#include <string>

// Triangle depth-sort weights INSIDE one model. We draw through
// SDL_RenderGeometry with no depth buffer, so overlapping pieces of a model need
// an order.
//
// DERIVED FROM THE PROJECTION, not from a camera angle. collect() emits
// position.y = -(kProjY*y + kProjZ*z) = -(0.5y + z), so two points land on the
// same pixel when 0.5*dy + dz = 0, i.e. along (dy,dz) = (2,-1). THAT is the view
// ray, and toward the camera is +y, -z: up is nearer, and model +z reads
// up-screen, which is away.
//
// So depth from the camera goes as (z - 2y), and the sort runs descending,
// farthest first.
//
// Getting here took two wrong turns worth remembering. The weights started as
// cos/sin of a 0.72 rad "tilt" the projection does not have (ratio 1.14 instead
// of 2). Then I "fixed" the SIGN as well, reasoning from retail's own
// screenY = z - y/2 -- but our code negates ry, so our z runs the other way, and
// flipping it put every z-separated pair in backwards order. The original signs
// were right; only the magnitudes were wrong.
//
// Retail does not sort at all. Its software path draws each model into its own
// image with an 8-bit depth plane keyed on vertex height (icd 0x4d47e0,
// 0x4ed2d0, 0x5415a0); the Glide path's order inside a model is not traced
// (docs/retail-engine.md, "Retail does not sort 3DO primitives"). So this is an
// approximation of a per-model depth test -- correct ordering between
// triangles, still unable to resolve two that interpenetrate.
inline constexpr float kSortY = 2.0f;   // weight on model Y along the view ray
inline constexpr float kSortZ = 1.0f;   // weight on model Z (opposite sign to Y)

// Retail's 2.5D projection, straight off the instruction sequence at icd
// 0x421dad:
//     screenX = x - cameraX
//     screenY = z - (y >> 1) - cameraY
// so screen Y takes ALL of z and HALF of y, and screen X takes no y at all.
// Note this is not a rotation -- the coefficients are 1.0 and 0.5, which are not
// the cosine and sine of any angle -- so it cannot be expressed as a tilt. We
// modelled it as one for a long time (cos/sin of gTilt = 0.75/0.66) and were
// wrong in both terms. The same halving governs the terrain lift: 0x511140
// returns a cell's height byte and 0x426820 subtracts height>>1.
inline constexpr float kProjY = 0.5f;   // screen-Y per unit of model Y
inline constexpr float kProjZ = 1.0f;   // screen-Y per unit of model Z

// The resolved retail install folder. The VFS covers everything inside the .hpi
// archives, but the movies are LOOSE files under <install>/Movies -- so anything
// that plays one (the front end, the loading screen) needs the path itself.
inline std::string gInstallRoot;
