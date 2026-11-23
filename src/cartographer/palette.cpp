// The left browser: section / feature / unit / start lists with search,
// category and faction filters, the thumbnail grid, and its input handling.

#include "cartographer/editorstate.h"
#include "cartographer/dialog.h"

const cart::FeatSprite* cart::Editor::featureSpriteFor(const cart::FeatureRef& ref) {
    return interactive?features.requestSprite(vfs,ref):features.sprite(vfs,ref);
}

SDL_Texture* cart::Editor::featTextureFor(const cart::FeatureRef& r) {
    auto it = featTex.find(r.name);
    if (it != featTex.end()) return it->second;
    SDL_Texture* t = nullptr;
    const cart::FeatSprite* sp = featureSpriteFor(r);
    if(!sp)return nullptr; // Pending work must not become a cached failure.
    if (sp && sp->w > 0) {
        t = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                              sp->w, sp->h);
        if (t) {
            SDL_UpdateTexture(t, nullptr, sp->rgba.data(), sp->w * 4);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        }
    }
    featTex[r.name] = t;
    return t;
}

SDL_Texture* cart::Editor::thumbFor(const std::string& path) {return thumbs.get(path);}

const tak::sim::UnitType* cart::Editor::unitInfo(int index) {return index>=0 && index<int(unitTypes.size())?unitRegistry.find(lowerText(unitTypes[size_t(index)])):nullptr;}

std::string cart::Editor::unitCategory(int index) {
    return cart::unitGroupOf(unitInfo(index));
}

std::string cart::Editor::paletteLabel(int index) {
    if(tool==UNITS) {const auto* info=unitInfo(index);return info?info->name+" ("+unitTypes[size_t(index)]+")":unitTypes[size_t(index)];}
    if(tool==FEATURES)return features.list()[size_t(index)].name;
    if(tool==STARTS)return "PLAYER "+std::to_string(scenario.starts[size_t(index)].number);
    if(tool==BLOCKED) {
        const auto size=std::to_string(cart::kBlockedBrushSizes[size_t(index)]);
        return "BRUSH "+size+" X "+size+(index?" CELLS":" CELL");
    }
    const auto& section=sections.list()[size_t(index)];
    if(catalogWorld!=world || sectionCatalog.pieceOf(index)<0)return section.name;
    return sectionCatalog.label(index)+"   "+section.name+" ("+section.category+")";
}

void cart::Editor::refreshPalette() {
    if(tool==TERRAIN && (catalogWorld!=world || catalogSize!=sections.list().size() || catalogGeneration!=sections.generation())) {
        sectionCatalog.build(sections.list());catalogWorld=world;catalogSize=sections.list().size();catalogGeneration=sections.generation();
        const auto remembered=preferences.sectionGroups.find(lowerText(world));   // retail "Zhon", new maps "zhon"
        sectionGroup=remembered!=preferences.sectionGroups.end() && sectionCatalog.group(remembered->second)?remembered->second:"";
        sectionMenuOpen=false;
    }
    const auto key=std::to_string(int(tool))+"/"+std::to_string(categoryIndex)+"/"+std::to_string(factionIndex)+"/"+world+"/"+paletteSearch+"/"+std::to_string(scenario.starts.size())+"/"+sectionGroup+"/"+std::to_string(catalogSize)+
        "/"+std::to_string(features.generation())+"/"+std::to_string(sections.generation());   // a rescan voids every row index
    if(key==paletteKey)return;
    const bool toolChanged=paletteKey.empty() || paletteKey[0]!=key[0];
    paletteKey=key;
    // A different list starts at the top; the same one growing or shrinking (a start
    // placed or undone) keeps its place, clamped when drawn.
    const auto listKey=std::to_string(int(tool))+"/"+std::to_string(categoryIndex)+"/"+std::to_string(factionIndex)+"/"+world+"/"+paletteSearch+"/"+sectionGroup;
    if(listKey!=paletteListKey) {paletteListKey=listKey;paletteScroll=0;paletteThumbDrag=false;}
    if(tool==TERRAIN) {
        // One row per piece; search spans every group, keeps a piece when any
        // variant matches and shows that variant (cycling still steps through all).
        paletteItems.clear();paletteVariants.clear();const auto query=lowerText(paletteSearch);
        const auto* group=query.empty()?sectionCatalog.group(sectionGroup):nullptr;
        if(!group)group=&sectionCatalog.groups().front();
        for(const int p:group->pieces) {
            const auto& variants=sectionCatalog.pieces()[size_t(p)].variants;
            int shown=-1;bool selectedMatches=false;
            for(const int v:variants)if(query.empty() || sectionCatalog.searchText(v).find(query)!=std::string::npos) {
                if(shown<0)shown=v;
                selectedMatches|=v==selected;
            }
            if(shown<0)continue;
            paletteItems.push_back(selectedMatches?selected:shown);paletteVariants.push_back(variants);
        }
        return;
    }
    const int count=tool==UNITS?int(unitTypes.size()):tool==FEATURES?int(features.list().size()):tool==STARTS?int(scenario.starts.size()):
        tool==BLOCKED?int(cart::kBlockedBrushSizes.size()):int(sections.list().size());
    auto category=[&](int i) {return tool==UNITS?unitCategory(i):tool==FEATURES?features.list()[size_t(i)].category:tool==STARTS?std::string("STARTS"):
        tool==BLOCKED?std::string("BRUSH"):sections.list()[size_t(i)].category;};
    std::set<std::string> categories;for(int i=0;i<count;++i)categories.insert(category(i));
    paletteCategories={"ALL"};paletteCategories.insert(paletteCategories.end(),categories.begin(),categories.end());
    if(toolChanged || categoryIndex>=int(paletteCategories.size()))categoryIndex=0;
    paletteItems.clear();const auto query=lowerText(paletteSearch);
    static const char* sides[]={"","ara","tar","ver","zon","cre"};
    for(int i=0;i<count;++i) {
        if(categoryIndex && category(i)!=paletteCategories[size_t(categoryIndex)])continue;
        if(tool==UNITS && factionIndex && !lowerText(unitTypes[size_t(i)]).starts_with(sides[factionIndex]))continue;
        // BLOCKED's five fixed brushes ignore the shared search: a word left from another
        // browser would otherwise empty the list with nothing on screen saying why.
        if(tool!=BLOCKED && !query.empty() && lowerText(paletteLabel(i)+" "+category(i)).find(query)==std::string::npos)continue;
        paletteItems.push_back(i);
    }
}

int cart::Editor::paletteContentHeight() const {
    const int count=int(paletteItems.size());
    if(tool==UNITS || tool==STARTS || tool==BLOCKED)return count*26;
    return count?(count+cols-1)/cols*kCellH+4:0;   // grid cells start 4px below the list top
}

// The track runs down the browser's right margin, beside the two thumbnail columns.
cart::ScrollBar cart::Editor::paletteScrollBar() const {
    ScrollBar bar;bar.x=kPaletteW-10;bar.w=8;bar.y=kPaletteTop;bar.h=std::max(0,h-kStatusH-kPaletteTop);
    bar.content=paletteContentHeight();bar.view=bar.h;return bar;
}

int cart::Editor::paletteRowAt(int x,int y) const {
    if(sectionMenuOpen && sectionMenuRowAt(x,y)>=0)return -1;
    if(paletteScrollBar().inTrack(x,y))return -1;
    if(tool==UNITS || tool==STARTS || tool==BLOCKED)return (y-kPaletteTop+paletteScroll)/26;
    const int column=(x-4)/(kThumb+4);   // the right margin is no cell (it read as the next row)
    return column<cols?((y-kPaletteTop-4+paletteScroll)/kCellH)*cols+column:-1;   // cells are drawn 4px down
}

std::string cart::Editor::paletteHint(int y) const {
    if(y<kMenuH+30)return "Search this browser by name or identifier (Ctrl+F). Escape returns keyboard focus to the map.";
    if(tool==TERRAIN)return y<kMenuH+53?"Click to choose a section group: a kind (cliffs, transitions, water...) or one of its retail folders. Remembered per world; search looks in every group.":
        "Numbered variants of a piece share one thumbnail: click the selected piece again to step through them. Hover shows the internal name.";
    if(y<kMenuH+53)return "Click to cycle browser categories or unit roles. Search and filters work together.";
    if(tool==UNITS)return "Click to cycle faction filters. Internal unit identifiers remain available in the browser details.";
    if(tool==BLOCKED)return "Blocked cells on this map. The rows below are brush sizes for painting or erasing them; [ and ] change it too.";
    return {};
}

// Search focus, Ctrl+F and the filter buttons above the list.
bool cart::Editor::handlePaletteFocus(SDL_Event& e) {
    if(handleSectionMenu(e))return true;
    if(e.type==SDL_KEYDOWN && (e.key.keysym.mod&(KMOD_CTRL|KMOD_GUI)) && e.key.keysym.sym==SDLK_f) {
        paletteFocus=true;paletteEditor.focus(paletteSearch);SDL_StartTextInput();return true;
    }
    if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
        if(e.button.x<kPaletteW && e.button.y>=kMenuH && e.button.y<kPaletteTop) {
            if(e.button.y<kMenuH+30) {paletteFocus=true;paletteEditor.focus(paletteSearch);SDL_StartTextInput();}
            else if(e.button.y<kMenuH+53 && tool==TERRAIN) {
                sectionMenuOpen=true;sectionMenuScroll=0;
                const auto& groups=sectionCatalog.groups();
                const int visible=std::max(1,(h-kStatusH-kMenuH-53)/kSectionRowH),last=std::max(0,int(groups.size())-visible);
                for(size_t i=0;i<groups.size();++i)if(groups[i].id==sectionGroup)sectionMenuScroll=std::clamp(int(i)-4,0,last)*kSectionRowH;
            }
            else if(e.button.y<kMenuH+53) {categoryIndex=(categoryIndex+1)%int(paletteCategories.size());refreshPalette();}
            else if(tool==UNITS) {factionIndex=(factionIndex+1)%6;refreshPalette();}
            return true;
        }
        if(paletteFocus) {paletteFocus=false;SDL_StopTextInput();}
    }
    if(paletteFocus && (e.type==SDL_TEXTINPUT || e.type==SDL_KEYDOWN)) {
        if(e.type==SDL_KEYDOWN && (e.key.keysym.sym==SDLK_ESCAPE || e.key.keysym.sym==SDLK_RETURN)) {paletteFocus=false;SDL_StopTextInput();}
        else {paletteEditor.input(e,paletteSearch);refreshPalette();}
        return true;
    }
    return false;
}

bool cart::Editor::handlePaletteClick(SDL_Event& e) {
    if(!(e.type == SDL_MOUSEBUTTONDOWN &&
         e.button.button == SDL_BUTTON_LEFT && e.button.x < kPaletteW &&
         e.button.y >= kPaletteTop && e.button.y < h - kStatusH))return false;
    const int row=paletteRowAt(e.button.x,e.button.y);
    if(row>=0 && row<int(paletteItems.size())) {
        regionCanvas=false;
        revealTool();const int index=paletteItems[size_t(row)];
        if(tool==UNITS)selectedType=index;
        else if(tool==FEATURES)selectedFeat=index;
        else if(tool==BLOCKED)blockedBrush=index;
        else if(tool==STARTS)mapView.setOffset(scenario.starts[size_t(index)].xpos*16-canvasW/(2*mapView.zoom()),scenario.starts[size_t(index)].zpos*16-canvasH/(2*mapView.zoom()));
        else if(index==selected && row<int(paletteVariants.size()) && paletteVariants[size_t(row)].size()>1) {
            // Clicking the selected piece again steps to its next variant.
            const auto& variants=paletteVariants[size_t(row)];
            const auto at=std::find(variants.begin(),variants.end(),index);
            selected=paletteItems[size_t(row)]=at==variants.end() || at+1==variants.end()?variants.front():*(at+1);
        }
        else selected=index;
    }
    return true;
}

bool cart::Editor::handlePaletteWheel(SDL_Event& e) {
    if(e.type != SDL_MOUSEWHEEL)return false;
    int mxp, myp; SDL_GetMouseState(&mxp, &myp); mxp /= kUIScale; myp /= kUIScale;
    if (mxp >= kPaletteW)return false;   // the canvas zooms instead
    paletteScroll=paletteScrollBar().clamp(paletteScroll-e.wheel.y*40);
    return true;
}

// Thumb drags own the pointer until release, so dragging onto the canvas never paints.
bool cart::Editor::handlePaletteScrollbar(SDL_Event& e) {
    const auto bar=paletteScrollBar();
    if(paletteThumbDrag) {
        if(e.type==SDL_MOUSEMOTION) {
            if(!(e.motion.state&SDL_BUTTON_LMASK))paletteThumbDrag=false;   // released outside the window
            else paletteScroll=bar.scrollForThumb(e.motion.y-paletteThumbGrab);
            return true;
        }
        if(e.type==SDL_MOUSEBUTTONUP && e.button.button==SDL_BUTTON_LEFT) {paletteThumbDrag=false;return true;}
        if(e.type==SDL_MOUSEBUTTONDOWN)return true;
        return false;
    }
    if(sectionMenuOpen || e.type!=SDL_MOUSEBUTTONDOWN || e.button.button!=SDL_BUTTON_LEFT || !bar.inTrack(e.button.x,e.button.y))return false;
    if(bar.inThumb(e.button.x,e.button.y,paletteScroll)) {paletteThumbDrag=true;paletteThumbGrab=e.button.y-bar.thumbTop(paletteScroll);}
    else paletteScroll=bar.page(paletteScroll,e.button.y);
    if(paletteFocus) {paletteFocus=false;SDL_StopTextInput();}
    return true;
}

void cart::Editor::drawPalette() {
    shotPending=0;
    fillRect(ren, 0, kMenuH, kPaletteW, canvasH, 30, 32, 40);
    const auto bar=paletteScrollBar();
    paletteScroll=bar.clamp(paletteScroll);   // the list shrank or the window got taller
    SDL_Rect palClip{0,kPaletteTop,bar.active()?bar.x-2:kPaletteW,h-kStatusH-kPaletteTop};
    SDL_RenderSetClipRect(ren,&palClip);
    if(tool==UNITS || tool==STARTS || tool==BLOCKED) {
        for(int row=0;row<int(paletteItems.size());++row) {
            const int i=paletteItems[size_t(row)],y=kPaletteTop+row*26-paletteScroll;
            if(y+24<kPaletteTop || y>h-kStatusH)continue;
            if((tool==UNITS && i==selectedType) || (tool==BLOCKED && i==blockedBrush))fillRect(ren,0,y,kPaletteW,25,70,66,40);
            if(tool==BLOCKED) {   // a red square sized like the brush, then its label
                const int side=std::min(22,6+cart::kBlockedBrushSizes[size_t(i)]);
                fillRect(ren,4+(32-side)/2,y+1+(24-side)/2,side,side,230,50,50);
                cart::drawText(ren,paletteLabel(i),42,y+9,1,235,230,200);
                continue;
            }
            if(tool==UNITS) {
                const auto* info=unitInfo(i);
                const auto portraitPath="anims/buildpic/"+lowerText(unitTypes[size_t(i)])+".jpg";
                auto* portrait=unitPortraits.get(portraitPath);
                if(!portrait && !shotPath.empty() && !unitPortraits.resolved(portraitPath))++shotPending;
                if(portrait) {
                    int pw=0,ph=0;SDL_QueryTexture(portrait,nullptr,nullptr,&pw,&ph);
                    if(pw>0 && ph>0) {
                        const float scale=std::min(32.f/pw,24.f/ph);
                        SDL_Rect dst{4+(32-int(pw*scale))/2,y+1+(24-int(ph*scale))/2,int(pw*scale),int(ph*scale)};
                        SDL_RenderCopy(ren,portrait,nullptr,&dst);
                    }
                }
                cart::drawText(ren,info?info->name:unitTypes[size_t(i)],42,y+3,1,235,230,200);
                cart::drawText(ren,unitTypes[size_t(i)]+" - "+unitCategory(i),42,y+14,1,160,175,195);
            } else cart::drawText(ren,paletteLabel(i),6,y+7,1,235,210,100);
        }
    } else {
        const int selection=tool==FEATURES?selectedFeat:selected;
        for(int row=0;row<int(paletteItems.size());++row) {
            const int i=paletteItems[size_t(row)],x=4+(row%cols)*(kThumb+4),y=kPaletteTop+4+(row/cols)*kCellH-paletteScroll;
            if(y+kCellH<kPaletteTop || y>h-kStatusH)continue;
            SDL_Rect cell{x,y,kThumb,kThumb};fillRect(ren,x,y,kThumb,kThumb,44,46,54);
            auto* texture=tool==FEATURES?featTextureFor(features.list()[size_t(i)]):thumbFor(sections.list()[size_t(i)].path);
            if(!texture && tool!=FEATURES && !shotPath.empty() && !thumbs.resolved(sections.list()[size_t(i)].path))++shotPending;
            if(texture) {
                if(tool==FEATURES) {
                    const auto* sprite=featureSpriteFor(features.list()[size_t(i)]);
                    const float scale=std::min(float(kThumb)/std::max(sprite->w,1),float(kThumb)/std::max(sprite->h,1));
                    SDL_Rect fit{x+(kThumb-int(sprite->w*scale))/2,y+(kThumb-int(sprite->h*scale))/2,int(sprite->w*scale),int(sprite->h*scale)};
                    SDL_RenderCopy(ren,texture,nullptr,&fit);
                } else SDL_RenderCopy(ren,texture,nullptr,&cell);
            }
            if(i==selection) {SDL_SetRenderDrawColor(ren,255,210,90,255);SDL_RenderDrawRect(ren,&cell);}
            if(tool==TERRAIN && row<int(paletteVariants.size()) && paletteVariants[size_t(row)].size()>1) {
                const auto& variants=paletteVariants[size_t(row)];
                const auto badge=std::to_string(std::find(variants.begin(),variants.end(),i)-variants.begin()+1)+"/"+std::to_string(variants.size());
                const int bw=cart::textWidth(badge,1)+6;
                fillRect(ren,x+kThumb-bw-2,y+2,bw,13,20,22,28);
                cart::drawText(ren,badge,x+kThumb-bw+1,y+5,1,255,220,120);
            }
            cart::drawText(ren,tool==TERRAIN && catalogWorld==world?sectionCatalog.shortLabel(i):cart::clipColumns(paletteLabel(i),14),x,y+kThumb+4,1,210,210,215);
        }
    }
    SDL_RenderSetClipRect(ren,nullptr);
    if(bar.active()) {   // track, then the thumb (brighter while hovered or dragged)
        int mx,my;SDL_GetMouseState(&mx,&my);mx=int(mx/kUIScale);my=int(my/kUIScale);
        const bool hot=paletteThumbDrag || (!sectionMenuOpen && bar.inThumb(mx,my,paletteScroll));
        fillRect(ren,bar.x,bar.y,bar.w,bar.h,22,24,30);
        fillRect(ren,bar.x+1,bar.thumbTop(paletteScroll)+1,bar.w-2,bar.thumbHeight()-2,hot?140:96,hot?150:104,hot?185:132);
    }
    cart::drawField(ren,4,kMenuH+3,kPaletteW-8,"SEARCH (CTRL+F)",paletteSearch,paletteFocus,paletteFocus?&paletteEditor:nullptr);
    if(tool==TERRAIN) {
        // Long folder names shorten; the count and the arrow always show.
        const auto* group=sectionCatalog.group(sectionGroup);
        std::string label=!paletteSearch.empty()?"Search: all groups":group?group->label:"All sections";
        const std::string tail=" ("+std::to_string(group && paletteSearch.empty()?group->sections:int(sections.list().size()))+") \xe2\x96\xbe";
        label=cart::clipColumns(label,std::max(1,(kPaletteW-20)/6-cart::textColumns(tail)));
        cart::drawButton(ren,4,kMenuH+33,kPaletteW-8,18,label+tail,sectionMenuOpen);
    } else cart::drawButton(ren,4,kMenuH+33,kPaletteW-8,18,cart::clipColumns(paletteCategories[size_t(categoryIndex)],28),false);
    if(tool!=TERRAIN)sectionMenuOpen=false;
    if(tool==TERRAIN) {
        size_t count=0;for(const auto& variants:paletteVariants)count+=variants.size();
        cart::drawText(ren,std::to_string(paletteItems.size())+" PIECES \xc2\xb7 "+std::to_string(count)+" SECTIONS",6,kMenuH+59,1,180,190,200);
    } else if(tool==UNITS) {
        static const char* factionNames[]={"ALL FACTIONS","ARAMON","TAROS","VERUNA","ZHON","CREON"};
        cart::drawButton(ren,4,kMenuH+54,kPaletteW-8,18,factionNames[factionIndex],false);
    } else if(tool==BLOCKED)cart::drawText(ren,"BLOCKED CELLS: "+std::to_string(blockedCount),6,kMenuH+59,1,180,190,200);
    else cart::drawText(ren,std::to_string(paletteItems.size())+" MATCHES",6,kMenuH+59,1,180,190,200);
}

// The group list drops down from the category button over the browser and canvas
// (drawn after the chrome so the panel edges stay underneath it).
void cart::Editor::selectSectionGroup(const std::string& id) {
    sectionGroup=id;sectionMenuOpen=false;
    if(!paletteSearch.empty()) {paletteSearch.clear();paletteEditor.focus(paletteSearch);}   // a search would hide the pick
    preferences.sectionGroups[lowerText(world)]=id;persistPreferences();
    refreshPalette();
}

int cart::Editor::sectionMenuRowAt(int x,int y) const {
    const int top=kMenuH+53;
    if(x<4 || x>=4+kSectionMenuW || y<top+2 || y>=h-kStatusH)return -1;
    const int row=(y-top-2+sectionMenuScroll)/kSectionRowH;   // rows are drawn 2px below the frame
    return row<int(sectionCatalog.groups().size())?row:-1;
}

bool cart::Editor::handleSectionMenu(SDL_Event& e) {
    if(sectionMenuPress) {   // the press that picked or dismissed: its drag and release are spent
        if(e.type==SDL_MOUSEBUTTONDOWN)sectionMenuPress=false;
        else if(e.type==SDL_MOUSEBUTTONUP) {sectionMenuPress=false;return true;}
        else if(e.type==SDL_MOUSEMOTION && (e.motion.state&(SDL_BUTTON_LMASK|SDL_BUTTON_RMASK)))return true;
    }
    if(!sectionMenuOpen)return false;
    if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE) {sectionMenuOpen=false;return true;}
    if(e.type==SDL_MOUSEWHEEL) {
        const int visible=std::max(1,(h-kStatusH-kMenuH-53)/kSectionRowH);
        const int total=int(sectionCatalog.groups().size());
        sectionMenuScroll=std::clamp(sectionMenuScroll-e.wheel.y*kSectionRowH*3,0,std::max(0,total-visible)*kSectionRowH);
        return true;
    }
    if(e.type==SDL_MOUSEBUTTONDOWN) {
        const int row=sectionMenuRowAt(e.button.x,e.button.y);
        if(row>=0 && e.button.button==SDL_BUTTON_LEFT)selectSectionGroup(sectionCatalog.groups()[size_t(row)].id);
        else sectionMenuOpen=false;   // a click elsewhere only closes the list
        sectionMenuPress=true;return true;
    }
    return false;
}

void cart::Editor::drawSectionMenu() {
    if(menuOpen>=0 || modal!=M_NONE || scriptOpen || aiOpen || useOnlyOpen || regionsOpen)sectionMenuOpen=false;   // they take over input
    if(!sectionMenuOpen || tool!=TERRAIN)return;
    const int top=kMenuH+53,bottom=h-kStatusH;
    const auto& groups=sectionCatalog.groups();
    const int height=std::min(bottom-top,int(groups.size())*kSectionRowH+4);
    fillRect(ren,4,top,kSectionMenuW,height,24,26,34);
    SDL_Rect frame{4,top,kSectionMenuW,height};SDL_SetRenderDrawColor(ren,110,115,130,255);SDL_RenderDrawRect(ren,&frame);
    SDL_Rect clip{5,top+1,kSectionMenuW-2,height-2};SDL_RenderSetClipRect(ren,&clip);
    int mx,my;SDL_GetMouseState(&mx,&my);mx=int(mx/kUIScale);my=int(my/kUIScale);
    const int hover=sectionMenuRowAt(mx,my);
    for(int row=0;row<int(groups.size());++row) {
        const int y=top+2+row*kSectionRowH-sectionMenuScroll;
        if(y+kSectionRowH<top || y>bottom)continue;
        const auto& g=groups[size_t(row)];
        if(g.id==sectionGroup)fillRect(ren,6,y,kSectionMenuW-4,kSectionRowH,70,66,40);
        else if(row==hover)fillRect(ren,6,y,kSectionMenuW-4,kSectionRowH,44,48,62);
        const std::string count=std::to_string(g.sections);
        const int indent=g.depth?20:8,room=(kSectionMenuW-indent-cart::textWidth(count,1)-16)/6;
        std::string label=g.label;if(!g.depth)for(char& c:label)c=char(std::toupper((unsigned char)c));
        cart::drawText(ren,cart::clipColumns(label,std::max(0,room)),indent,y+4,1,g.depth?200:255,g.depth?205:225,g.depth?215:150);
        cart::drawText(ren,count,4+kSectionMenuW-8-cart::textWidth(count,1),y+4,1,150,160,175);
    }
    SDL_RenderSetClipRect(ren,nullptr);
}
