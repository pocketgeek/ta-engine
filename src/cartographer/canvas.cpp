// The map canvas: terrain stamping, feature / unit / start placement, select /
// erase / pan modes, region drawing, Clear Area, and everything drawn on the map.

#include "cartographer/editorstate.h"
#include "cartographer/dialog.h"

#include <cmath>

namespace {
// 8 player marker colours.
const Uint8 kPlayerCol[9][3] = {
    {80,120,255},{230,70,60},{80,200,90},{235,210,80},
    {200,90,220},{80,210,220},{240,140,40},{200,200,210},{150,150,150}};
}

// Stamp a named section into the map at block (bx,by), returning whether it
// matched a section. The editor calls this on canvas click; --stamp tests it.
bool cart::Session::stampByName(const std::string& name, int bx, int by) {
    MapView& mapView=*mapViewPtr;
    for (const auto& s : sections.list())
        if (s.name == name) {
            const tak::tnt::Map* sec = sections.load(vfs, s.path);
            if (sec && cart::stampSection(mapView.editMap(), *sec, bx, by)) {
                mapView.tilesEdited(bx,by,sec->blocksX,sec->blocksY);
                edited = true; dirty = true; historyPending=true;
                return true;
            }
        }
    return false;
}

void cart::Editor::stampAtMouse(int mx, int my, int w, int h) {
    if (mx >= kPaletteW && clipboardBrush()) { pasteTerrainAtMouse(mx, my); return; }
    if (selected < 0 || mx < kPaletteW) return;   // palette side, not the canvas
    const auto& s = sections.list()[size_t(selected)];
    const tak::tnt::Map* sec = sections.load(vfs, s.path);
    if (!sec || sec->blocksX <= 0) return;
    // Canvas-local -> world -> block, snapped to the section's own size so
    // sections tile cleanly.
    float lx = float(mx - kPaletteW), ly = float(my - kMenuH);
    int blkX = int((mapView.offX() + lx / mapView.zoom()) / 32.0f);
    int blkY = int((mapView.offY() + ly / mapView.zoom()) / 32.0f);
    int snapX = (blkX / sec->blocksX) * sec->blocksX;
    int snapY = (blkY / sec->blocksY) * sec->blocksY;
    if(snapX==lastStampX && snapY==lastStampY)return;
    lastStampX=snapX;lastStampY=snapY;
    // Painting changes tile arrays only. Workers use tile snapshots and
    // immutable source sections; no whole-map worker/cache reset is needed.
    if (cart::stampSection(mapView.editMap(), *sec, snapX, snapY,stampLayers)) {
        mapView.tilesEdited(snapX,snapY,sec->blocksX,sec->blocksY);invalidateMinimap();
        edited = true; dirty = true; historyPending=true;
        stampMirrors(selected,snapX,snapY);   // live symmetry: twin sections
    }
    (void)w; (void)h;
}

int cart::Editor::unitAt(int mx, int my) {
    for (int i = int(units.size()) - 1; i >= 0; --i) {
        float sx = kPaletteW + (units[i].x - mapView.offX()) * mapView.zoom();
        float sy = kMenuH + (units[i].z - mapView.offY()) * mapView.zoom();
        if (std::abs(sx - mx) <= 8 && std::abs(sy - my) <= 8) return i;
    }
    return -1;
}

bool cart::Editor::toolVisible() {return tool==TERRAIN || tool==BLOCKED || (tool==FEATURES?showFeatures:tool==UNITS?showUnits:showStarts);}

void cart::Editor::revealTool() {
    bool* visible=tool==FEATURES?&showFeatures:tool==UNITS?&showUnits:tool==STARTS?&showStarts:nullptr;
    if(visible && !*visible) {*visible=true;persistPreferences();}
}

void cart::Editor::placeFeature(int mx, int my, bool erase) {
    if (selectedFeat < 0 || mx < kPaletteW) return;
    float wx = mapView.offX() + float(mx - kPaletteW) / mapView.zoom();
    float wz = mapView.offY() + float(my - kMenuH) / mapView.zoom();
    int cx = int(wx / 16.0f), cz = int(wz / 16.0f);
    auto& mp = mapView.editMap();
    if (cx < 0 || cz < 0 || cx >= mp.width || cz >= mp.height) return;
    size_t ci = size_t(cz) * mp.width + cx;
    if (erase) { mp.features[ci] = 0xFFFF; edited = true; dirty = true; historyPending=true; return; }
    const std::string& name = features.list()[size_t(selectedFeat)].name;
    uint16_t idx = 0xFFFF;   // intern the feature name into the map's table
    for (size_t i = 0; i < mp.featureNames.size(); ++i)
        if (mp.featureNames[i] == name) { idx = uint16_t(i); break; }
    if (idx == 0xFFFF) { mp.featureNames.push_back(name); idx = uint16_t(mp.featureNames.size() - 1); }
    mp.features[ci] = idx;
    edited = true; dirty = true; historyPending=true;
}

bool cart::Editor::mouseCell(int mx, int my, int& cx, int& cz) {
    if (mx < kPaletteW) return false;
    float wx = mapView.offX() + float(mx - kPaletteW) / mapView.zoom();
    float wz = mapView.offY() + float(my - kMenuH) / mapView.zoom();
    cx = int(wx / 16.0f); cz = int(wz / 16.0f);
    return cx >= 0 && cz >= 0 && cx < mapView.map().width && cz < mapView.map().height;
}

// The BLOCKED tool in Place (paint) or Erase mode: a drag strokes the brush along
// the pointer, Shift+drag fills a rectangle on release. Only empty cells take a
// blocker and only blockers are erased (blockers.h). No tile changes, so the
// minimap and the saved minimaps stay as they are: `edited` is not set.
bool cart::Editor::handleBlockedInput(SDL_Event& e) {
    if(tool!=BLOCKED || regionCanvas || clearArm || (editMode!=MODE_PLACE && editMode!=MODE_ERASE)) {blockedStroke=blockedRect=false;return false;}
    const auto op=editMode==MODE_PLACE?cart::BlockOp::Paint:cart::BlockOp::Erase;
    const int size=cart::kBlockedBrushSizes[size_t(std::clamp(blockedBrush,0,int(cart::kBlockedBrushSizes.size())-1))];
    const auto& map=mapView.map();
    auto onCanvas=[&](int x,int y) {return x>=kPaletteW && y>=kMenuH && y<h-kStatusH;};
    auto corner=[&](int x,int y) {   // a rectangle's far corner follows the pointer anywhere, kept on the map
        blockedRectX1=std::clamp(int(std::floor((mapView.offX()+(x-kPaletteW)/mapView.zoom())/16)),0,map.width-1);
        blockedRectZ1=std::clamp(int(std::floor((mapView.offY()+(y-kMenuH)/mapView.zoom())/16)),0,map.height-1);
    };
    auto stroke=[&](int x,int z) {
        if(cart::applyBlockerLine(mapView.editMap(),blockedLastX<0?x:blockedLastX,blockedLastX<0?z:blockedLastZ,x,z,size,op))dirty=historyPending=true;
        blockedLastX=x;blockedLastZ=z;
    };
    if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT && onCanvas(e.button.x,e.button.y)) {
        int cx,cz;blockedStroke=blockedRect=false;blockedLastX=-1;
        const bool shift=SDL_GetModState()&KMOD_SHIFT;
        if(!mouseCell(e.button.x,e.button.y,cx,cz)) {blockedStroke=!shift;return true;}   // past the map edge: the stroke starts once the pointer reaches the map
        if(shift) {blockedRect=true;blockedRectX0=blockedRectX1=cx;blockedRectZ0=blockedRectZ1=cz;}
        else {blockedStroke=true;stroke(cx,cz);}
        return true;
    }
    if(e.type==SDL_MOUSEMOTION && (e.motion.state&SDL_BUTTON_LMASK) && (blockedStroke || blockedRect)) {
        int cx,cz;
        if(blockedRect)corner(e.motion.x,e.motion.y);
        else if(!onCanvas(e.motion.x,e.motion.y))blockedLastX=-1;   // off the canvas: the stroke resumes where the pointer comes back, without a line
        else if(mouseCell(e.motion.x,e.motion.y,cx,cz))stroke(cx,cz);   // past the map edge the last cell is kept, so a wall along the edge rejoins without a hole
        return true;
    }
    if(e.type==SDL_MOUSEBUTTONUP && e.button.button==SDL_BUTTON_LEFT && (blockedStroke || blockedRect)) {
        if(blockedRect) {
            corner(e.button.x,e.button.y);
            if(cart::applyBlockers(mapView.editMap(),blockedRectX0,blockedRectZ0,blockedRectX1,blockedRectZ1,op))dirty=historyPending=true;
        }
        blockedStroke=blockedRect=false;blockedLastX=-1;
        return true;
    }
    return false;
}

int cart::Editor::startAt(int mx, int my) {
    for (int i = 0; i < int(scenario.starts.size()); ++i) {
        float sx = kPaletteW + (scenario.starts[i].xpos * 16.0f - mapView.offX()) * mapView.zoom();
        float sy = kMenuH + (scenario.starts[i].zpos * 16.0f - mapView.offY()) * mapView.zoom();
        if (std::abs(sx - mx) <= 10 && std::abs(sy - my) <= 10) return i;
    }
    return -1;
}

// An active region move/resize/create drag owns the pointer until release.
bool cart::Editor::handleRegionDrag(SDL_Event& e) {
    if(!regionDrag.active())return false;
    if(e.type==SDL_QUIT || (e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE) ||
       (e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_RIGHT) ||
       (e.type==SDL_WINDOWEVENT && e.window.event==SDL_WINDOWEVENT_FOCUS_LOST)) {
        regionDrag.cancel();
        if(e.type!=SDL_QUIT)return true;
    } else if(e.type==SDL_MOUSEMOTION || (e.type==SDL_MOUSEBUTTONUP && e.button.button==SDL_BUTTON_LEFT)) {
        const int x=e.type==SDL_MOUSEMOTION?e.motion.x:e.button.x,z=e.type==SDL_MOUSEMOTION?e.motion.y:e.button.y;
        regionDrag.update((mapView.offX()+(x-kPaletteW)/mapView.zoom())/16,
                          (mapView.offY()+(z-kMenuH)/mapView.zoom())/16,mapView.map().width,mapView.map().height);
        if(e.type==SDL_MOUSEBUTTONUP) {
            const int index=regionDrag.index;std::string error;
            const auto& a=regionDrag.original;const auto& b=regionDrag.preview;
            if(index>=0 && a.x1==b.x1 && a.x2==b.x2 && a.z1==b.z1 && a.z2==b.z2) {regionDrag.cancel();return true;}
            if(cart::setRegion(scen,index,regionDrag.preview,mapView.map().width,mapView.map().height,error)) {
                regionSelected=index<0?int(scen.regions.size())-1:index;historyPending=true;
                commitHistory(); // unchanged click does not dirty the document
                if(index<0)editMode=MODE_SELECT;
            } else openMessage("REGION NOT CHANGED",error);
            regionDrag.cancel();
        }
        return true;
    } else return true;
    return false;
}

// Region-canvas keys and the feature / unit clipboard keys.
bool cart::Editor::handleCanvasKeys(SDL_Event& e) {
    const LiveEdit live(*this,e,true);
    if(handleTerrainKeys(e))return true;
    // BLOCKED: [ and ] step the brush size (for the other tools they pick the
    // placement player); Esc drops a rectangle still being dragged.
    if(e.type==SDL_KEYDOWN && tool==BLOCKED && !(e.key.keysym.mod&(KMOD_CTRL|KMOD_GUI))) {
        const auto key=e.key.keysym.sym;
        if(key==SDLK_LEFTBRACKET || key==SDLK_RIGHTBRACKET) {
            blockedBrush=std::clamp(blockedBrush+(key==SDLK_RIGHTBRACKET?1:-1),0,int(cart::kBlockedBrushSizes.size())-1);return true;
        }
        if(key==SDLK_ESCAPE && blockedRect) {blockedRect=false;return true;}
    }
    if(regionCanvas && e.type==SDL_KEYDOWN) {
        if(e.key.keysym.sym==SDLK_ESCAPE) {regionCanvas=false;return true;}
        if(e.key.keysym.sym==SDLK_RETURN && regionSelected>=0 && regionSelected<int(scen.regions.size())) {openModal(M_REGION,regionSelected);return true;}
        if(e.key.keysym.sym==SDLK_DELETE) {
            std::string error;
            if(cart::removeRegion(scen,regionSelected,error)) {regionSelected=-1;dirty=true;historyPending=true;}
            else openMessage("REGION NOT DELETED",error);
            return true;
        }
    }
    if(e.type==SDL_KEYDOWN && tool==FEATURES && showFeatures && !regionCanvas) {
        const auto key=e.key.keysym.sym;const bool ctrl=e.key.keysym.mod&(KMOD_CTRL|KMOD_GUI);
        if(ctrl && (key==SDLK_c || key==SDLK_x || key==SDLK_d))selectedFeatures.copy(mapView.map());
        if(key==SDLK_DELETE || (ctrl && key==SDLK_x)) {
            if(selectedFeatures.remove(mapView.editMap())) {edited=dirty=historyPending=true;}return true;
        }
        if(ctrl && (key==SDLK_v || key==SDLK_d)) {
            int mx,my;SDL_GetMouseState(&mx,&my);int cx=0,cz=0;
            if(!mouseCell(int(mx/kUIScale),int(my/kUIScale),cx,cz)) {cx=int((mapView.offX()+canvasW/(2*mapView.zoom()))/16);cz=int((mapView.offY()+canvasH/(2*mapView.zoom()))/16);}
            if(selectedFeatures.paste(mapView.editMap(),cx,cz)) {edited=dirty=historyPending=true;editMode=MODE_SELECT;}
            else if(!selectedFeatures.clipboard.empty())openMessage("FEATURES NOT PASTED","Choose an empty area large enough for the whole group. Existing features and terrain markers are protected.");
            return true;
        }
        if(ctrl && key==SDLK_c)return true;
        if(key==SDLK_RETURN && !selectedFeatures.cells.empty()) {
            std::string info=std::to_string(selectedFeatures.cells.size())+" selected feature(s).";
            for(int i:selectedFeatures.cells)if(cart::FeatureSelection::selectable(mapView.map(),i)) {
                info+="\n"+mapView.map().featureNames[mapView.map().features[i]]+" ("+std::to_string(i%mapView.map().width)+", "+std::to_string(i/mapView.map().width)+")";
                if(info.size()>700) {info+="\n...";break;}
            }
            openMessage("SELECTED FEATURES",info);return true;
        }
    }
    if(e.type==SDL_KEYDOWN && tool==UNITS && showUnits && !regionCanvas) {
        const auto key=e.key.keysym.sym;const bool ctrl=e.key.keysym.mod&(KMOD_CTRL|KMOD_GUI);
        if(ctrl && (key==SDLK_c || key==SDLK_x || key==SDLK_d))selectedUnits.copy(units);
        if(key==SDLK_DELETE || (ctrl && key==SDLK_x)) {
            if(selectedUnits.remove(units)) {dirty=true;historyPending=true;unitsEdited=true;}return true;
        }
        if(ctrl && (key==SDLK_v || key==SDLK_d)) {
            int mx,my;SDL_GetMouseState(&mx,&my);int cx=0,cz=0;
            if(!mouseCell(int(mx/kUIScale),int(my/kUIScale),cx,cz)) {cx=int((mapView.offX()+canvasW/(2*mapView.zoom()))/16);cz=int((mapView.offY()+canvasH/(2*mapView.zoom()))/16);}
            if(selectedUnits.paste(units,cx*16+8,cz*16+8,mapView.map().width*16,mapView.map().height*16)) {dirty=true;historyPending=true;unitsEdited=true;editMode=MODE_SELECT;}
            return true;
        }
        if(ctrl && key==SDLK_c)return true;
        if(key==SDLK_RETURN && !selectedUnits.indices.empty()) {openModal(M_UNIT,*selectedUnits.indices.begin());return true;}
    }
    return false;
}

// Canvas zoom, clicks, drags and right-drag panning; other events reach MapView.
void cart::Editor::handleCanvasInput(SDL_Event& e,float pointerDX,float pointerDZ) {
    const LiveEdit live(*this,e,false);
    if(handleTerrainSelection(e) || handleBlockedInput(e))return;
    if (e.type == SDL_MOUSEWHEEL) {
        int mxp, myp; SDL_GetMouseState(&mxp, &myp); mxp /= kUIScale; myp /= kUIScale;
        const float before=mapView.zoom();
        const float next=std::clamp(before*(e.wheel.y>0?1.2f:1.0f/1.2f),.025f,4.0f);
        mapView.setOffset(mapView.offX()+(mxp-kPaletteW)*(1/before-1/next),
                          mapView.offY()+(myp-kMenuH)*(1/before-1/next));
        mapView.setZoom(next);
    } else if (e.type == SDL_MOUSEBUTTONDOWN &&
               e.button.button == SDL_BUTTON_LEFT && e.button.x >= kPaletteW && e.button.y>=kMenuH && e.button.y<h-kStatusH) {
        if(editMode==MODE_PAN)return;
        if(!regionCanvas && !toolVisible())return;
        if(regionCanvas) {
            const float x=(mapView.offX()+(e.button.x-kPaletteW)/mapView.zoom())/16;
            const float z=(mapView.offY()+(e.button.y-kMenuH)/mapView.zoom())/16;
            if(regionDrag.begin(scen.regions,regionSelected,x,z,5/(mapView.zoom()*16),editMode==MODE_PLACE,mapView.map().width,mapView.map().height)) {
                regionSelected=regionDrag.index;
                if(editMode==MODE_ERASE) {
                    regionDrag.cancel();std::string error;
                    if(cart::removeRegion(scen,regionSelected,error)) {regionSelected=-1;dirty=true;historyPending=true;}
                    else openMessage("REGION NOT DELETED",error);
                } else if(e.button.clicks>=2 && regionSelected>=0) {regionDrag.cancel();openModal(M_REGION,regionSelected);}
            } else regionSelected=-1;
            return;
        }
        if(editMode==MODE_ERASE) {
            if(tool==FEATURES)placeFeature(e.button.x,e.button.y,true);
            else if(tool==UNITS) {const int hit=unitAt(e.button.x,e.button.y);if(hit>=0) {selectedUnits.indices={hit};selectedUnits.remove(units);dirty=true;historyPending=true;}}
            else if(tool==STARTS) {const int hit=startAt(e.button.x,e.button.y);if(hit>=0) {scenario.starts.erase(scenario.starts.begin()+hit);dirty=true;historyPending=true;}}
            return;
        }
        if(editMode==MODE_SELECT && tool!=UNITS) {
            if(tool==STARTS)draggingStart=startAt(e.button.x,e.button.y);
            if(tool==FEATURES) {
                int cx,cz;if(mouseCell(e.button.x,e.button.y,cx,cz)) {
                    const int i=cz*mapView.map().width+cx;
                    if(cart::FeatureSelection::selectable(mapView.map(),i)) {
                        selectedFeatures.click(i,SDL_GetModState()&KMOD_SHIFT);
                        selectedFeatures.dragging=true;selectedFeatures.dragX=cx;selectedFeatures.dragZ=cz;
                    } else {selectionBox=true;selectionAppend=SDL_GetModState()&KMOD_SHIFT;selectionX0=selectionX1=e.button.x;selectionZ0=selectionZ1=e.button.y;}
                }
            }
            return;
        }
        if(editMode==MODE_SELECT && tool==UNITS) {
            const int hit=unitAt(e.button.x,e.button.y);int cx,cz;
            if(mouseCell(e.button.x,e.button.y,cx,cz)) {
                if(hit>=0) {
                    selectedUnits.click(hit,SDL_GetModState()&KMOD_SHIFT);
                    if(e.button.clicks>=2)openModal(M_UNIT,hit);
                    else selectedUnits.beginDrag(units,cx*16+8,cz*16+8);
                } else {selectionBox=true;selectionAppend=SDL_GetModState()&KMOD_SHIFT;selectionX0=selectionX1=e.button.x;selectionZ0=selectionZ1=e.button.y;}
            }
            return;
        }
        if (clearArm) {   // Clear Area: begin the drag box
            clearDrag = true;
            clx0 = clx1 = e.button.x; cly0 = cly1 = e.button.y;
        } else if (tool == TERRAIN) {
            stampAtMouse(e.button.x, e.button.y, canvasW, canvasH);
        } else if (tool == FEATURES) {
            placeFeature(e.button.x, e.button.y, false);
        } else if (tool == UNITS) {   // grab an existing unit, else place one
            int hit = unitAt(e.button.x, e.button.y);
            int cx, cz;
            if (hit >= 0 && e.button.clicks >= 2) {
                selectedUnits.indices={hit};openModal(M_UNIT, hit);   // double-click: edit properties
            } else if (hit >= 0) {
                selectedUnits.indices={hit};draggingUnit = hit;
            } else if (selectedType >= 0 && mouseCell(e.button.x, e.button.y, cx, cz)) {
                cart::PlacedUnit u;
                u.type = unitTypes[size_t(selectedType)];
                u.player = currentPlayer;
                std::string id=u.type;std::transform(id.begin(),id.end(),id.begin(),[](unsigned char c){return char(std::tolower(c));});
                if(const auto* type=unitRegistry.find(id)) {u.footX=type->footX;u.footZ=type->footZ;}
                u.x = cx * 16.0f + u.footX*8; u.z = cz * 16.0f + u.footZ*8;
                units.push_back(u);
                draggingUnit = int(units.size()) - 1;selectedUnits.indices={draggingUnit};
                unitsEdited = true; dirty = true; historyPending=true;
            }
        } else if (tool == STARTS) {   // grab an existing marker, else place a new one
            int hit = startAt(e.button.x, e.button.y);
            int cx, cz;
            if (hit >= 0) draggingStart = hit;
            else if (mouseCell(e.button.x, e.button.y, cx, cz)) {
                // Next free StartPos1..8: a match seats at most eight players, and
                // Check Map rejects any other number.
                if (const int num = cart::nextStartNumber(scenario.starts)) {
                    scenario.starts.push_back({num, cx, cz}); dirty = true; historyPending=true;
                    draggingStart = int(scenario.starts.size()) - 1;
                } else openMessage("START POSITIONS FULL", "All eight start positions are on the map; a match seats at most eight players. Move or erase one first.");
            }
        }
    } else if (e.type == SDL_MOUSEBUTTONUP && e.button.button == SDL_BUTTON_LEFT) {
        if(selectionBox) {
            auto wx=[&](int x){return mapView.offX()+(x-kPaletteW)/mapView.zoom();};
            auto wz=[&](int z){return mapView.offY()+(z-kMenuH)/mapView.zoom();};
            if(tool==FEATURES)selectedFeatures.box(mapView.map(),int(std::floor(wx(selectionX0)/16)),int(std::floor(wz(selectionZ0)/16)),int(std::floor(wx(selectionX1)/16)),int(std::floor(wz(selectionZ1)/16)),selectionAppend);
            else selectedUnits.box(units,wx(selectionX0),wz(selectionZ0),wx(selectionX1),wz(selectionZ1),selectionAppend);
            selectionBox=false;
        }
        selectedUnits.dragOrigins.clear();selectedFeatures.dragging=false;
        lastStampX=lastStampY=-1;
        draggingStart = -1; draggingUnit = -1;
        if (clearDrag) {   // Clear Area: confirm, then remove units + features
            clearDrag = false;
            int a0, b0, a1, b1;
            if (mouseCell(std::min(clx0, clx1), std::min(cly0, cly1), a0, b0) &&
                mouseCell(std::max(clx0, clx1), std::max(cly0, cly1), a1, b1)) {
                int lox = std::min(a0, a1), hix = std::max(a0, a1);
                int loz = std::min(b0, b1), hiz = std::max(b0, b1);
                int nUnits = 0;
                for (const auto& u : units) {
                    int ux = int(u.x / 16.0f), uz = int(u.z / 16.0f);
                    if (ux >= lox && ux <= hix && uz >= loz && uz <= hiz) ++nUnits;
                }
                int cells = (hix - lox + 1) * (hiz - loz + 1);
                openConfirm("CLEAR AREA",
                            "Remove all units and features in this area?  (" +
                                std::to_string(nUnits) + " unit(s), " +
                                std::to_string(cells) + " cells)",
                            [this, lox, hix, loz, hiz]() {
                    auto& m = mapView.editMap();
                    for (int z = loz; z <= hiz; ++z)
                        for (int x = lox; x <= hix; ++x)
                            m.features[size_t(z) * m.width + x] = 0xFFFF;
                    units.erase(std::remove_if(units.begin(), units.end(),
                        [&](const cart::PlacedUnit& u) {
                            int ux = int(u.x / 16.0f), uz = int(u.z / 16.0f);
                            return ux >= lox && ux <= hix && uz >= loz && uz <= hiz;
                        }), units.end());
                    selectedUnits.indices.clear();selectedFeatures.clear();
                    edited = true; unitsEdited = true; dirty = true; historyPending=true;
                });
            }
            clearArm = false;
        }
    } else if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_LMASK) &&
               e.motion.x >= kPaletteW) {
        if(editMode==MODE_PAN) {mapView.setOffset(mapView.offX()-pointerDX/mapView.zoom(),mapView.offY()-pointerDZ/mapView.zoom());return;}
        if(regionCanvas || !toolVisible())return;
        if(editMode==MODE_ERASE) {if(tool==FEATURES)placeFeature(e.motion.x,e.motion.y,true);return;}
        if(editMode==MODE_SELECT && tool!=UNITS) {
            int cx,cz;
            if(tool==FEATURES) {
                if(selectionBox) {selectionX1=e.motion.x;selectionZ1=e.motion.y;}
                else if(mouseCell(e.motion.x,e.motion.y,cx,cz) && selectedFeatures.drag(mapView.editMap(),cx,cz))edited=dirty=historyPending=true;
            }
            if(tool==STARTS && draggingStart>=0 && mouseCell(e.motion.x,e.motion.y,cx,cz)) {
                scenario.starts[size_t(draggingStart)].xpos=cx;scenario.starts[size_t(draggingStart)].zpos=cz;dirty=true;historyPending=true;
            }
            return;
        }
        if(editMode==MODE_SELECT && tool==UNITS) {
            if(selectionBox) {selectionX1=e.motion.x;selectionZ1=e.motion.y;}
            else {int cx,cz;if(mouseCell(e.motion.x,e.motion.y,cx,cz) && selectedUnits.drag(units,cx*16+8,cz*16+8,mapView.map().width*16,mapView.map().height*16)) {dirty=true;historyPending=true;}}
            return;
        }
        if (clearDrag) {   // Clear Area: grow the box
            clx1 = e.motion.x; cly1 = e.motion.y;
        } else if (tool == TERRAIN) {
            if (!clipboardBrush()) stampAtMouse(e.motion.x, e.motion.y, canvasW, canvasH);   // drag-paint; the clipboard pastes per click
        } else if (tool == FEATURES) {
            placeFeature(e.motion.x, e.motion.y, false);   // drag-place features
        } else if (tool == UNITS && draggingUnit >= 0) {
            int cx, cz;   // drag a unit to a new cell centre
            if (mouseCell(e.motion.x, e.motion.y, cx, cz)) {
                units[size_t(draggingUnit)].x = cx * 16.0f + units[size_t(draggingUnit)].footX*8;
                units[size_t(draggingUnit)].z = cz * 16.0f + units[size_t(draggingUnit)].footZ*8;
                unitsEdited = true; dirty = true; historyPending=true;
            }
        } else if (draggingStart >= 0) {
            int cx, cz;   // drag a start marker to a new cell
            if (mouseCell(e.motion.x, e.motion.y, cx, cz)) {
                scenario.starts[size_t(draggingStart)].xpos = cx;
                scenario.starts[size_t(draggingStart)].zpos = cz; dirty = true; historyPending=true;
            }
        }
    } else if (e.type == SDL_MOUSEMOTION && (e.motion.state & SDL_BUTTON_RMASK)) {
        mapView.setOffset(mapView.offX()-pointerDX/mapView.zoom(),mapView.offY()-pointerDZ/mapView.zoom());
    } else if (e.type != SDL_MOUSEBUTTONDOWN && e.type != SDL_MOUSEMOTION) {
        mapView.input(e);
    }
}

void cart::Editor::drawCanvas() {
    // Map canvas (right of the palette, between menu and status).
    SDL_Rect canvas{kPaletteW, kMenuH, canvasW, canvasH};
    SDL_RenderSetViewport(ren, &canvas);
    mapView.draw(canvasW, canvasH);
    if(overlayTexture) {
        SDL_FRect destination{-mapView.offX()*mapView.zoom(),-mapView.offY()*mapView.zoom(),overlayWidth*16*mapView.zoom(),overlayHeight*16*mapView.zoom()};
        SDL_RenderCopyF(ren,overlayTexture.get(),nullptr,&destination);
        fillRect(ren,4,4,std::min(canvasW-8,cart::textWidth(overlayLegend,1)+8),17,15,20,30);
        cart::drawText(ren,overlayLegend,8,9,1,240,240,210);
    }
    drawBlocked();
    // Grid overlay: section (512px) lines bright, block (32px) lines faint.
    if (showGrid) {
        float zm = mapView.zoom();
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        int firstBlkX = int(mapView.offX()) / 32, firstBlkY = int(mapView.offY()) / 32;
        for (int bxg = firstBlkX; ; ++bxg) {
            float sx = (bxg * 32 - mapView.offX()) * zm;
            if (sx > canvasW) break;
            bool section = (bxg % 16) == 0;
            SDL_SetRenderDrawColor(ren, 255, 255, 255, section ? 90 : 30);
            SDL_RenderDrawLine(ren, int(sx), 0, int(sx), canvasH);
        }
        for (int byg = firstBlkY; ; ++byg) {
            float sy = (byg * 32 - mapView.offY()) * zm;
            if (sy > canvasH) break;
            bool section = (byg % 16) == 0;
            SDL_SetRenderDrawColor(ren, 255, 255, 255, section ? 90 : 30);
            SDL_RenderDrawLine(ren, 0, int(sy), canvasW, int(sy));
        }
    }
    auto drawRegion=[&](const tak::crt::Region& region,bool selected) {
        const float zoom=mapView.zoom();
        SDL_FRect box{(std::min(region.x1,region.x2)*16-mapView.offX())*zoom,(std::min(region.z1,region.z2)*16-mapView.offY())*zoom,
            (std::abs(region.x2-region.x1)+1)*16*zoom,(std::abs(region.z2-region.z1)+1)*16*zoom};
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);SDL_SetRenderDrawColor(ren,80,190,245,45);SDL_RenderFillRectF(ren,&box);
        SDL_SetRenderDrawColor(ren,selected?255:120,selected?220:215,selected?100:255,230);SDL_RenderDrawRectF(ren,&box);
        if(selected)for(int y=0;y<3;++y)for(int x=0;x<3;++x)if(x!=1 || y!=1) {
            SDL_FRect handle{box.x+box.w*x/2-3,box.y+box.h*y/2-3,6,6};SDL_RenderFillRectF(ren,&handle);
        }
        cart::drawText(ren,region.name,int(box.x)+3,int(box.y)+3,1,180,235,255);
    };
    if(showRegions || regionCanvas) {
        for(int i=0;i<int(scen.regions.size());++i)
            drawRegion(regionDrag.active() && regionDrag.index==i?regionDrag.preview:scen.regions[i],regionCanvas && regionSelected==i);
        if(regionDrag.active() && regionDrag.index<0)drawRegion(regionDrag.preview,true);
    }
    // Placed features: draw each non-empty feature-plane cell's sprite at its
    // cell, anchored like the game. Culled to the visible canvas.
    if(showFeatures) {
        const auto& mp = mapView.map();
        float zm = mapView.zoom();
        const int minX=std::max(0,int((mapView.offX()-512)/16));
        const int minZ=std::max(0,int((mapView.offY()-512)/16));
        const int maxX=std::min(mp.width,int((mapView.offX()+canvasW/zm+512)/16)+1);
        const int maxZ=std::min(mp.height,int((mapView.offY()+canvasH/zm+512)/16)+1);
        for (int cz = minZ; cz < maxZ; ++cz)
            for (int cx = minX; cx < maxX; ++cx) {
                uint16_t v = mp.features[size_t(cz) * mp.width + cx];
                if (v >= 0xFFFA || v >= mp.featureNames.size()) continue;
                float bx = (cx * 16.0f - mapView.offX()) * zm;
                float by = (cz * 16.0f - mapView.offY()) * zm;
                if (bx < -64 || by < -64 || bx > canvasW + 64 || by > canvasH + 64) continue;
                // A name no feature file of this world defines (another world's, or
                // a downloaded map's own) still occupies the cell: mark it in red so
                // it can be seen, selected and erased.
                const cart::FeatureRef* r = features.byName(mp.featureNames[v]);
                SDL_Texture* t = r ? featTextureFor(*r) : nullptr;
                const cart::FeatSprite* sp = r ? featureSpriteFor(*r) : nullptr;
                if (!t || !sp || sp->w == 0) {   // no art: a small marker
                    if (r) SDL_SetRenderDrawColor(ren, 90, 200, 90, 220);
                    else SDL_SetRenderDrawColor(ren, 235, 70, 60, 230);
                    SDL_Rect dot{int(bx) - 2, int(by) - 2, 4, 4};
                    SDL_RenderFillRect(ren, &dot);
                    continue;
                }
                SDL_FRect dst{bx - sp->xoff * zm, by - sp->yoff * zm,
                              sp->w * zm, sp->h * zm};
                SDL_RenderCopyF(ren, t, nullptr, &dst);
            }
    }
    if(showFeatures && tool==FEATURES)for(int i:selectedFeatures.cells)if(cart::FeatureSelection::selectable(mapView.map(),i)) {
        const float zoom=mapView.zoom();
        SDL_FRect cell{(i%mapView.map().width*16-mapView.offX())*zoom,(i/mapView.map().width*16-mapView.offY())*zoom,16*zoom,16*zoom};
        SDL_SetRenderDrawColor(ren,255,225,100,255);SDL_RenderDrawRectF(ren,&cell);
    }
    // Placed units: a player-coloured square with the type name above it.
    if(showUnits)for (int i = 0; i < int(units.size()); ++i) {
        float sx = (units[i].x - mapView.offX()) * mapView.zoom();
        float sy = (units[i].z - mapView.offY()) * mapView.zoom();
        if (sx < -20 || sy < -20 || sx > canvasW + 20 || sy > canvasH + 20) continue;
        const Uint8* pc = kPlayerCol[std::clamp(units[i].player,0,8)];
        SDL_SetRenderDrawColor(ren, pc[0], pc[1], pc[2], 255);
        SDL_Rect r{int(sx) - 5, int(sy) - 5, 10, 10};
        SDL_RenderFillRect(ren, &r);
        SDL_SetRenderDrawColor(ren, 12, 12, 16, 255);
        SDL_RenderDrawRect(ren, &r);
        if(selectedUnits.contains(i)) {
            const auto* type=unitRegistry.find(lowerText(units[i].type));
            const float width=(type?type->footX:1)*16*mapView.zoom(),height=(type?type->footZ:1)*16*mapView.zoom();
            SDL_SetRenderDrawColor(ren,255,225,100,255);
            SDL_FRect footprint{sx-width/2,sy-height/2,width,height};SDL_RenderDrawRectF(ren,&footprint);
            const float angle=units[i].angle*3.14159265f/180;
            // CRT angle a faces (sin a, cos a) in map x/z, as the engine spawns it.
            SDL_RenderDrawLineF(ren,sx,sy,sx+std::sin(angle)*22,sy+std::cos(angle)*22);
        }
        if (mapView.zoom() > 0.28f) {  // label only when there's room
            const auto& label=units[i].name.empty()?units[i].type:units[i].name;
            cart::drawText(ren, label,
                           int(sx) - cart::textWidth(label, 1) / 2,
                           int(sy) - 15, 1, 235, 235, 245);
        }
    }
    if(selectionBox) {
        SDL_SetRenderDrawColor(ren,255,220,110,255);
        SDL_Rect area{std::min(selectionX0,selectionX1)-kPaletteW,std::min(selectionZ0,selectionZ1)-kMenuH,std::abs(selectionX1-selectionX0),std::abs(selectionZ1-selectionZ0)};
        SDL_RenderDrawRect(ren,&area);
    }
    // Start-position markers (drawn in canvas-local coords: gold diamonds
    // with the StartPos number). Off-map ones simply fall outside.
    if(showStarts)for (int i = 0; i < int(scenario.starts.size()); ++i) {
        float sx = (scenario.starts[i].xpos * 16.0f - mapView.offX()) * mapView.zoom();
        float sy = (scenario.starts[i].zpos * 16.0f - mapView.offY()) * mapView.zoom();
        if (sx < -12 || sy < -12 || sx > canvasW + 12 || sy > canvasH + 12) continue;
        SDL_Vertex d[4] = {
            {{sx, sy - 9}, {255, 205, 70, 255}, {0, 0}},
            {{sx + 9, sy}, {255, 205, 70, 255}, {0, 0}},
            {{sx, sy + 9}, {255, 205, 70, 255}, {0, 0}},
            {{sx - 9, sy}, {255, 205, 70, 255}, {0, 0}},
        };
        int di[6] = {0, 1, 2, 0, 2, 3};
        SDL_RenderGeometry(ren, nullptr, d, 4, di, 6);
        SDL_SetRenderDrawColor(ren, 30, 20, 0, 255);
        SDL_Rect out{int(sx) - 9, int(sy) - 9, 18, 18};
        SDL_RenderDrawRect(ren, &out);
        cart::drawText(ren, std::to_string(scenario.starts[i].number),
                       int(sx) - 2, int(sy) - 3, 1, 20, 14, 0);
    }
    // Preview the actual snapped stamp / feature / footprint before committing.
    int previewX,previewZ;SDL_GetMouseState(&previewX,&previewZ);previewX=int(previewX/kUIScale);previewZ=int(previewZ/kUIScale);
    if(toolVisible() && tool!=BLOCKED && editMode==MODE_PLACE && !regionCanvas && modal==M_NONE && !scriptOpen && !aiOpen && !useOnlyOpen && !regionsOpen && menuOpen<0 &&
       previewX>=kPaletteW && previewZ>=kMenuH && previewZ<h-kStatusH && !cart::pointIn(previewX,previewZ,miniRect)) {
        const float wx=mapView.offX()+(previewX-kPaletteW)/mapView.zoom(),wz=mapView.offY()+(previewZ-kMenuH)/mapView.zoom();
        SDL_FRect box{};SDL_Texture* ghost=nullptr;
        if(tool==TERRAIN && clipboardBrush()) {
            const auto b=clipBrushBox(wx,wz);
            box={(b.x0*32-mapView.offX())*mapView.zoom(),(b.z0*32-mapView.offY())*mapView.zoom(),(b.x1-b.x0)*32*mapView.zoom(),(b.z1-b.z0)*32*mapView.zoom()};
            ghost=clipTexture.get();
        } else if(tool==TERRAIN && selected>=0) {
            const auto& section=sections.list()[size_t(selected)];const auto* map=sections.load(vfs,section.path);
            if(map && map->blocksX>0 && map->blocksY>0) {
                const int x=(int(wx/32)/map->blocksX)*map->blocksX,z=(int(wz/32)/map->blocksY)*map->blocksY;
                box={(x*32-mapView.offX())*mapView.zoom(),(z*32-mapView.offY())*mapView.zoom(),map->blocksX*32*mapView.zoom(),map->blocksY*32*mapView.zoom()};
                ghost=thumbFor(section.path);
            }
        } else if(tool==FEATURES && selectedFeat>=0) {
            const auto& feature=features.list()[size_t(selectedFeat)];
            ghost=featTextureFor(feature);
            const auto* sprite=featureSpriteFor(feature);
            if(sprite && sprite->w>0)box={(int(wx/16)*16-mapView.offX()-sprite->xoff)*mapView.zoom(),(int(wz/16)*16-mapView.offY()-sprite->yoff)*mapView.zoom(),sprite->w*mapView.zoom(),sprite->h*mapView.zoom()};
            else box={(int(wx/16)*16-mapView.offX())*mapView.zoom(),(int(wz/16)*16-mapView.offY())*mapView.zoom(),16*mapView.zoom(),16*mapView.zoom()};
        } else {
            const auto* type=tool==UNITS?unitInfo(selectedType):nullptr;
            const float width=(type?type->footX:1)*16*mapView.zoom(),height=(type?type->footZ:1)*16*mapView.zoom();
            box={(int(wx/16)*16+8-mapView.offX())*mapView.zoom()-width/2,(int(wz/16)*16+8-mapView.offY())*mapView.zoom()-height/2,width,height};
        }
        if(ghost) {Uint8 alpha=255;SDL_BlendMode blend;SDL_GetTextureAlphaMod(ghost,&alpha);SDL_GetTextureBlendMode(ghost,&blend);SDL_SetTextureAlphaMod(ghost,120);SDL_SetTextureBlendMode(ghost,SDL_BLENDMODE_BLEND);SDL_RenderCopyF(ren,ghost,nullptr,&box);SDL_SetTextureAlphaMod(ghost,alpha);SDL_SetTextureBlendMode(ghost,blend);}
        SDL_SetRenderDrawColor(ren,255,220,90,220);SDL_RenderDrawRectF(ren,&box);
    }
    // BLOCKED: the brush under the pointer (red paints, white erases), or the
    // rectangle a Shift+drag will fill.
    if(tool==BLOCKED && (editMode==MODE_PLACE || editMode==MODE_ERASE) && !regionCanvas && !clearArm && modal==M_NONE && !scriptOpen && !aiOpen && !useOnlyOpen && !regionsOpen && menuOpen<0) {
        const float zoom=mapView.zoom();
        auto cells=[&](int x0,int z0,int x1,int z1) {
            return SDL_FRect{(std::min(x0,x1)*16-mapView.offX())*zoom,(std::min(z0,z1)*16-mapView.offY())*zoom,(std::abs(x1-x0)+1)*16*zoom,(std::abs(z1-z0)+1)*16*zoom};
        };
        SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
        int cx,cz;
        if(blockedRect) {
            const auto box=cells(blockedRectX0,blockedRectZ0,blockedRectX1,blockedRectZ1);
            SDL_SetRenderDrawColor(ren,255,220,90,45);SDL_RenderFillRectF(ren,&box);
            SDL_SetRenderDrawColor(ren,255,220,90,235);SDL_RenderDrawRectF(ren,&box);
        } else if(previewX>=kPaletteW && previewZ>=kMenuH && previewZ<h-kStatusH && !cart::pointIn(previewX,previewZ,miniRect) && mouseCell(previewX,previewZ,cx,cz)) {
            const auto b=cart::brushCells(cx,cz,cart::kBlockedBrushSizes[size_t(std::clamp(blockedBrush,0,int(cart::kBlockedBrushSizes.size())-1))]);
            const auto box=cells(b.x0,b.z0,b.x1,b.z1);
            if(editMode==MODE_PLACE)SDL_SetRenderDrawColor(ren,255,90,80,235);else SDL_SetRenderDrawColor(ren,245,245,250,235);
            SDL_RenderDrawRectF(ren,&box);
        }
    }
    drawSymmetryOverlay();
    SDL_RenderSetViewport(ren, nullptr);
}

// The blocked-cell overlay follows the plane: compared only after an edit, an
// undo or a new document, and only the changed rows and columns are uploaded.
void cart::Editor::refreshBlockedOverlay() {
    const auto& map=mapView.map();
    if(map.width<=0 || map.height<=0 || map.features.size()!=size_t(map.width)*size_t(map.height))return;
    const bool resized=!blockedTexture || map.width!=blockedW || map.height!=blockedH;
    if(!resized && !historyPending && !resetHistory && history.revision()==blockedRevision)return;
    blockedRevision=history.revision();
    int x0=0,z0=0,x1=map.width-1,z1=map.height-1;
    if(resized) {
        blockedTexture.reset(SDL_CreateTexture(ren,SDL_PIXELFORMAT_RGBA32,SDL_TEXTUREACCESS_STREAMING,map.width,map.height));
        if(!blockedTexture)return;
        SDL_SetTextureBlendMode(blockedTexture.get(),SDL_BLENDMODE_BLEND);SDL_SetTextureScaleMode(blockedTexture.get(),SDL_ScaleModeNearest);
        blockedW=map.width;blockedH=map.height;blockedPlane=map.features;
    } else {
        x0=map.width;z0=map.height;x1=z1=-1;
        for(int z=0;z<map.height;++z) {
            const uint16_t* row=map.features.data()+size_t(z)*map.width;uint16_t* shown=blockedPlane.data()+size_t(z)*map.width;
            if(std::equal(row,row+map.width,shown))continue;
            for(int x=0;x<map.width;++x)if(row[x]!=shown[x]) {x0=std::min(x0,x);x1=std::max(x1,x);shown[x]=row[x];}
            z0=std::min(z0,z);z1=z;
        }
        if(x1<0)return;
    }
    blockedCount=cart::countBlocked(map);
    const int w=x1-x0+1,rows=z1-z0+1;
    std::vector<uint8_t> rgba(size_t(w)*rows*4,0);
    for(int z=z0;z<=z1;++z)for(int x=x0;x<=x1;++x) {
        const uint16_t v=map.features[size_t(z)*map.width+x];uint8_t* p=&rgba[(size_t(z-z0)*w+(x-x0))*4];
        if(v==cart::kCellBlocked) {p[0]=230;p[1]=50;p[2]=50;p[3]=140;}
        else if(v==cart::kCellRoad) {p[0]=80;p[1]=150;p[2]=240;p[3]=90;}   // why paint skipped a cell
    }
    const SDL_Rect area{x0,z0,w,rows};
    SDL_UpdateTexture(blockedTexture.get(),&area,rgba.data(),w*4);
}

// Blocked cells red and roads blue, under the features (View > Show blocked cells, B).
void cart::Editor::drawBlocked() {
    if(!blockedShown())return;
    refreshBlockedOverlay();
    if(!blockedTexture)return;
    const float zoom=mapView.zoom();
    const SDL_FRect destination{-mapView.offX()*zoom,-mapView.offY()*zoom,blockedW*16*zoom,blockedH*16*zoom};
    SDL_RenderCopyF(ren,blockedTexture.get(),nullptr,&destination);
}

void cart::Editor::drawClearArea() {
    // Clear Area drag box (screen-space rectangle).
    if (clearDrag) {
        SDL_Rect box{std::min(clx0, clx1), std::min(cly0, cly1),
                     std::abs(clx1 - clx0), std::abs(cly1 - cly0)};
        SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
        SDL_SetRenderDrawColor(ren, 230, 90, 80, 60); SDL_RenderFillRect(ren, &box);
        SDL_SetRenderDrawColor(ren, 240, 120, 100, 220); SDL_RenderDrawRect(ren, &box);
    }
}
