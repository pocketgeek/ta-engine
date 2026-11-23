#pragma once

// The lobby's full-window map view: a click on the room's or its CHANGE MAP panel's map
// preview shows the chosen map's own terrain and features (trees, rocks, mana spots,
// ruins) fitted to the window, instead of the small radar picture. GameView draws it
// over the lobby, which keeps running underneath, so a room still starts while a
// player is looking at the map. Display only: nothing here touches the game.

#include <SDL.h>

#include "cartographer/features.h"
#include "client/mapview.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace tak::hpi { class Vfs; }

class BigMapView {
public:
    // `vfs` is what the map is read through: a view over the game's data that holds a
    // downloaded or verified map's files. It is kept for this view's life (MapView and
    // the feature art borrow it). `world` (aramon, taros, ...) picks the feature files.
    BigMapView(SDL_Renderer* ren, std::unique_ptr<tak::hpi::Vfs> vfs, const std::string& tntPath,
               const std::string& world);
    BigMapView(SDL_Renderer* ren, std::unique_ptr<tak::hpi::Vfs> vfs, tak::tnt::Map map,
               const std::string& world);
    ~BigMapView();
    BigMapView(const BigMapView&) = delete;
    BigMapView& operator=(const BigMapView&) = delete;

    // Draw the whole map fitted and centred in `area` (output pixels, no render scale
    // or viewport set); returns the rectangle the map covers.
    SDL_FRect draw(const SDL_FRect& area);
    // The map's size in world pixels (16 per cell).
    float worldW() const;
    float worldH() const;

private:
    void loadFeatures(const std::string& world);

    SDL_Renderer* ren_;
    std::unique_ptr<tak::hpi::Vfs> vfs_;   // declared before view_: MapView borrows it
    std::unique_ptr<MapView> view_;
    cart::FeatureLibrary features_;
    struct Sprite { SDL_Texture* tex = nullptr; int w = 0, h = 0, xoff = 0, yoff = 0; };
    std::map<uint16_t, Sprite> sprites_;   // by the map's feature-name index
    struct Placed { float x, y; const Sprite* sprite; };
    std::vector<Placed> placed_;           // every drawn feature, at its cell's corner
};
