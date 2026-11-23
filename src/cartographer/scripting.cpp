// Scenario Scripting overlay: per-player trigger rule groups with their
// conditions and actions, the opcode picker and objective templates.

#include "cartographer/editorstate.h"
#include "cartographer/dialog.h"
#include "cartographer/triggers.h"

std::vector<tak::crt::RuleGroup>& cart::Editor::scrGroups() {
    return scen.players[size_t(scrPlayer)];
}

tak::crt::RuleGroup* cart::Editor::curGroup() {
    auto& gs = scrGroups();
    return (scrGroup >= 0 && scrGroup < int(gs.size())) ? &gs[size_t(scrGroup)] : nullptr;
}

void cart::Editor::ruleOperation(int operation) {
    auto& groups=scrGroups();
    int row=ruleColumn==cart::RuleColumn::Action?scrActSel:scrCondSel;
    bool changed=false;
    if(operation==0)ruleClipboard.copy(groups,scrGroup,row,ruleColumn);
    if(operation==1) {changed=ruleClipboard.paste(groups,scrGroup,row,ruleColumn);if(changed && ruleClipboard.column==cart::RuleColumn::Group)ruleColumn=cart::RuleColumn::Group;}
    if(operation==2) {cart::RuleClipboard duplicate;if(duplicate.copy(groups,scrGroup,row,ruleColumn))changed=duplicate.paste(groups,scrGroup,row,ruleColumn);}
    if(operation==3 || operation==4)changed=cart::moveRule(groups,scrGroup,row,ruleColumn,operation==3?-1:1);
    if(operation==5)ruleClipboard.copy(groups,scrGroup,row,ruleColumn,true);
    if(operation==6 && ruleClipboard.column==cart::RuleColumn::Group) {scrGroup=int(groups.size())-1;changed=ruleClipboard.paste(groups,scrGroup,row,cart::RuleColumn::Group);ruleColumn=cart::RuleColumn::Group;}
    if(changed) {
        if(ruleColumn==cart::RuleColumn::Action)scrActSel=row;
        else if(ruleColumn==cart::RuleColumn::Condition)scrCondSel=row;
        else scrCondSel=scrActSel=-1;
        auto reveal=[](int selected,int height,int& scroll) {
            if(selected<0)return;
            if(selected*12<scroll)scroll=selected*12;
            else if((selected+1)*12>scroll+height)scroll=std::max(0,(selected+1)*12-height);
        };
        reveal(scrGroup,rRuleList.h,scrRuleScroll);
        if(ruleColumn==cart::RuleColumn::Action)reveal(scrActSel,rActList.h,scrActScroll);
        if(ruleColumn==cart::RuleColumn::Condition)reveal(scrCondSel,rCondList.h,scrCondScroll);
        dirty=true;historyPending=true;
    }
}

void cart::Editor::openScripting() {
    if (int(scen.players.size()) < 9) scen.players.resize(9);   // retail writes 9
    scriptOpen = true; scrPlayer = 0;ruleColumn=cart::RuleColumn::Group;
    scrGroup = scen.players[0].empty() ? -1 : 0;
    scrCondSel = scrActSel = -1;
    scrRuleScroll = scrCondScroll = scrActScroll = 0;
}

void cart::Editor::openTemplates() {
    std::string type=unitTypes.empty()?"":unitTypes.front();
    for(int i:selectedUnits.indices)if(i>=0 && i<int(units.size())) {type=units[i].type;break;}
    std::string location=scen.regions.empty()?"Anywhere":scen.regions.front().name;
    if(regionSelected>=0 && regionSelected<int(scen.regions.size()))location=scen.regions[regionSelected].name;
    templateCatalog=cart::ruleTemplates(scrPlayer,type,location);
    templateQuery.clear();templateEditor.focus(templateQuery);templateSelection=0;
    pickOpen=pickTemplate=true;pickScroll=0;SDL_StartTextInput();
}

void cart::Editor::insertTemplate() {
    const auto matches=cart::matchingRuleTemplates(templateCatalog,templateQuery);
    if(templateSelection<0 || templateSelection>=int(matches.size()))return;
    cart::RuleClipboard item;item.groups={templateCatalog[matches[templateSelection]].group};
    int row=-1;
    if(!item.paste(scrGroups(),scrGroup,row,cart::RuleColumn::Group))return;
    ruleColumn=cart::RuleColumn::Group;scrCondSel=scrActSel=-1;
    scrRuleScroll=std::max(0,(scrGroup+1)*12-rRuleList.h);
    scrDetailScroll=0;dirty=historyPending=true;pickOpen=pickTemplate=false;SDL_StopTextInput();
}

// The Scripting (trigger) overlay swallows input while up.
void cart::Editor::handleScripting(SDL_Event& e) {
    constexpr int kRow = 12;
    if(pickOpen && pickTemplate) {
        const auto matches=cart::matchingRuleTemplates(templateCatalog,templateQuery);
        if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_ESCAPE) {pickOpen=pickTemplate=false;SDL_StopTextInput();}
        else if(e.type==SDL_KEYDOWN && e.key.keysym.sym==SDLK_RETURN)insertTemplate();
        else if(e.type==SDL_KEYDOWN && (e.key.keysym.sym==SDLK_UP || e.key.keysym.sym==SDLK_DOWN))
            templateSelection=std::clamp(templateSelection+(e.key.keysym.sym==SDLK_UP?-1:1),0,std::max(0,int(matches.size())-1));
        else if(e.type==SDL_TEXTINPUT || e.type==SDL_KEYDOWN) {
            if(templateEditor.input(e,templateQuery))templateSelection=0;
        } else if(e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
            const int mx=e.button.x,my=e.button.y;
            if(cart::pointIn(mx,my,rPickCancel)) {pickOpen=pickTemplate=false;SDL_StopTextInput();}
            else if(cart::pointIn(mx,my,rTemplateSearch))templateEditor.focus(templateQuery);
            else if(cart::pointIn(mx,my,rPickList)) {
                templateSelection=(my-rPickList.y)/42;insertTemplate();
            }
        }
        return;
    }
    if(!pickOpen && e.type==SDL_KEYDOWN) {
        const auto key=e.key.keysym.sym;const auto mod=e.key.keysym.mod;
        if(key==SDLK_PAGEUP || key==SDLK_PAGEDOWN) {scrDetailScroll=std::clamp(scrDetailScroll+(key==SDLK_PAGEUP?-6:6),0,scrDetailMax);return;}
        if(key==SDLK_RETURN) {
            if(auto* group=curGroup()) {
                if(ruleColumn==cart::RuleColumn::Group)openRuleName(group);
                if(ruleColumn==cart::RuleColumn::Condition && scrCondSel>=0 && scrCondSel<int(group->conditions.size()))openRuleEditor(&group->conditions[scrCondSel],false);
                if(ruleColumn==cart::RuleColumn::Action && scrActSel>=0 && scrActSel<int(group->actions.size()))openRuleEditor(&group->actions[scrActSel],true);
            }
            return;
        }
        if(mod&(KMOD_CTRL|KMOD_GUI)) {
            if(key==SDLK_c) {ruleOperation(0);return;}
            if(key==SDLK_v) {ruleOperation(1);return;}
            if(key==SDLK_d) {ruleOperation(2);return;}
        }
        if((mod&KMOD_ALT) && (key==SDLK_UP || key==SDLK_DOWN)) {ruleOperation(key==SDLK_UP?3:4);return;}
    }
    if(!pickOpen && e.type==SDL_MOUSEBUTTONDOWN && e.button.button==SDL_BUTTON_LEFT) {
        if(cart::pointIn(e.button.x,e.button.y,rTemplates)) {openTemplates();return;}
        if(cart::pointIn(e.button.x,e.button.y,rNameRule)) {openRuleName(curGroup());return;}
        if(cart::pointIn(e.button.x,e.button.y,rCopyRuleText)) {SDL_SetClipboardText(scrDetailText.c_str());return;}
        const SDL_Rect buttons[]={ruleCopy,rulePaste,ruleDuplicate,ruleUp,ruleDown,playerCopy,playerPaste};
        bool handled=false;
        for(int i=0;i<7;++i)if(cart::pointIn(e.button.x,e.button.y,buttons[i])) {ruleOperation(i);handled=true;break;}
        if(handled)return;
    }
    // Nested opcode picker (choose a condition/action type to add).
    if (pickOpen) {
        const auto& defs = pickAction ? cart::actionDefs() : cart::conditionDefs();
        if (e.type == SDL_MOUSEWHEEL) {
            int maxS = std::max(0, int(defs.size()) * kRow - rPickList.h);
            pickScroll = std::clamp(pickScroll - e.wheel.y * 36, 0, maxS);
        } else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
            pickOpen = false;
        } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
            int mx = e.button.x, my = e.button.y;
            if (cart::pointIn(mx, my, rPickCancel)) pickOpen = false;
            else if (cart::pointIn(mx, my, rPickList)) {
                int row = (my - rPickList.y + pickScroll) / kRow;
                tak::crt::RuleGroup* g = curGroup();
                if (g && row >= 0 && row < int(defs.size())) {
                    tak::crt::Rule r; r.opcode = row;
                    for (size_t i = 0; i < defs[size_t(row)].params.size() && i < 5; ++i)
                        r.slot[i] = cart::defaultParam(defs[size_t(row)].params[i]);
                    if (pickAction) { g->actions.push_back(r); scrActSel = int(g->actions.size()) - 1; dirty = true; historyPending=true; }
                    else { g->conditions.push_back(r); scrCondSel = int(g->conditions.size()) - 1; dirty = true; historyPending=true; }
                    pickOpen = false;
                }
            }
        }
        return;
    }
    auto& gs = scrGroups();
    tak::crt::RuleGroup* g = curGroup();
    if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
        scriptOpen = false;
    } else if (e.type == SDL_MOUSEWHEEL) {
        int mx, my; SDL_GetMouseState(&mx, &my); mx /= kUIScale; my /= kUIScale;
        if(cart::pointIn(mx,my,rRuleDetail))scrDetailScroll=std::clamp(scrDetailScroll-e.wheel.y*3,0,scrDetailMax);
        else if (cart::pointIn(mx, my, rRuleList)) {
            int maxS = std::max(0, int(gs.size()) * kRow - rRuleList.h);
            scrRuleScroll = std::clamp(scrRuleScroll - e.wheel.y * 36, 0, maxS);
        } else if (g && cart::pointIn(mx, my, rCondList)) {
            int maxS = std::max(0, int(g->conditions.size()) * kRow - rCondList.h);
            scrCondScroll = std::clamp(scrCondScroll - e.wheel.y * 36, 0, maxS);
        } else if (g && cart::pointIn(mx, my, rActList)) {
            int maxS = std::max(0, int(g->actions.size()) * kRow - rActList.h);
            scrActScroll = std::clamp(scrActScroll - e.wheel.y * 36, 0, maxS);
        }
    } else if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
        int mx = e.button.x, my = e.button.y;
        bool dbl = e.button.clicks >= 2;
        if (cart::pointIn(mx, my, rScrDone)) scriptOpen = false;
        else if (cart::pointIn(mx, my, rPrevP)) {
            ruleColumn=cart::RuleColumn::Group;scrPlayer = (scrPlayer + 8) % 9; scrGroup = scen.players[size_t(scrPlayer)].empty() ? -1 : 0;
            scrCondSel = scrActSel = -1; scrRuleScroll = scrCondScroll = scrActScroll = 0;
        } else if (cart::pointIn(mx, my, rNextP)) {
            ruleColumn=cart::RuleColumn::Group;scrPlayer = (scrPlayer + 1) % 9; scrGroup = scen.players[size_t(scrPlayer)].empty() ? -1 : 0;
            scrCondSel = scrActSel = -1; scrRuleScroll = scrCondScroll = scrActScroll = 0;
        } else if (cart::pointIn(mx, my, rAddRule)) {
            ruleColumn=cart::RuleColumn::Group;gs.push_back({}); scrGroup = int(gs.size()) - 1; scrCondSel = scrActSel = -1; dirty = true; historyPending=true;
        } else if (cart::pointIn(mx, my, rDelRule) && g) {
            gs.erase(gs.begin() + scrGroup); dirty = true; historyPending=true;
            scrGroup = gs.empty() ? -1 : std::min(scrGroup, int(gs.size()) - 1);
            scrCondSel = scrActSel = -1;
        } else if (cart::pointIn(mx, my, rAddCond)) {
            if (!g) { gs.push_back({}); scrGroup = int(gs.size()) - 1; dirty = true; historyPending=true; }
            pickOpen = true; pickTemplate=false; pickAction = false; pickScroll = 0;
        } else if (cart::pointIn(mx, my, rAddAct)) {
            if (!g) { gs.push_back({}); scrGroup = int(gs.size()) - 1; dirty = true; historyPending=true; }
            pickOpen = true; pickTemplate=false; pickAction = true; pickScroll = 0;
        } else if (cart::pointIn(mx, my, rDelCond) && g && scrCondSel >= 0 &&
                   scrCondSel < int(g->conditions.size())) {
            g->conditions.erase(g->conditions.begin() + scrCondSel); scrCondSel = -1; dirty = true; historyPending=true;
        } else if (cart::pointIn(mx, my, rDelAct) && g && scrActSel >= 0 &&
                   scrActSel < int(g->actions.size())) {
            g->actions.erase(g->actions.begin() + scrActSel); scrActSel = -1; dirty = true; historyPending=true;
        } else if (cart::pointIn(mx, my, rRuleList)) {
            int row = (my - rRuleList.y + scrRuleScroll) / kRow;
            if (row >= 0 && row < int(gs.size())) {
                scrGroup = row;ruleColumn=cart::RuleColumn::Group; scrCondSel = scrActSel = -1;
                scrCondScroll = scrActScroll = 0;
            }
        } else if (g && cart::pointIn(mx, my, rCondList)) {
            int row = (my - rCondList.y + scrCondScroll) / kRow;
            if (row >= 0 && row < int(g->conditions.size())) {
                scrCondSel = row;ruleColumn=cart::RuleColumn::Condition;
                if (dbl) openRuleEditor(&g->conditions[size_t(row)], false);
            }
        } else if (g && cart::pointIn(mx, my, rActList)) {
            int row = (my - rActList.y + scrActScroll) / kRow;
            if (row >= 0 && row < int(g->actions.size())) {
                scrActSel = row;ruleColumn=cart::RuleColumn::Action;
                if (dbl) openRuleEditor(&g->actions[size_t(row)], true);
            }
        }
    }
}

// Scripting (trigger) overlay: per-player rule groups + their
// conditions and actions. Drawn under the picker / param modal.
void cart::Editor::drawScripting() {
    if (scriptOpen) {
        constexpr int kRow = 12;
        SDL_Rect ct = cart::drawPanel(ren, w, h, 780, 520, "SCENARIO SCRIPTING");
        auto& gs = scen.players[size_t(scrPlayer)];
        tak::crt::RuleGroup* g = curGroup();
        // Header row: player nav, rule count, Done.
        rPrevP = cart::drawButton(ren, ct.x, ct.y, 18, 14, "<", false);
        cart::drawText(ren, scrPlayer==0 ? "ALL PLAYERS" : "PLAYER " + std::to_string(scrPlayer), ct.x + 24, ct.y + 3, 1, 220, 224, 235);
        rNextP = cart::drawButton(ren, ct.x + 128, ct.y, 18, 14, ">", false);
        cart::drawText(ren, std::to_string(gs.size()) + " RULES", ct.x + 156, ct.y + 3, 1, 175, 185, 200);
        rTemplates=cart::drawButton(ren,ct.x+244,ct.y,104,14,"TEMPLATES",false);
        rScrDone = cart::drawButton(ren, ct.x + ct.w - 60, ct.y, 56, 14, "DONE", true);

        int colW = (ct.w - 20) / 3;
        int x0 = ct.x, x1 = ct.x + colW + 10, x2 = ct.x + 2 * colW + 20;
        ruleCopy=cart::drawButton(ren,ct.x,ct.y+21,52,16,"COPY",false);
        rulePaste=cart::drawButton(ren,ct.x+58,ct.y+21,52,16,"PASTE",false);
        ruleDuplicate=cart::drawButton(ren,ct.x+116,ct.y+21,64,16,"DUPLICATE",false);
        ruleUp=cart::drawButton(ren,ct.x+186,ct.y+21,38,16,"UP",false);
        ruleDown=cart::drawButton(ren,ct.x+230,ct.y+21,38,16,"DOWN",false);
        playerCopy=cart::drawButton(ren,ct.x+274,ct.y+21,86,16,"COPY PLAYER",false);
        playerPaste=cart::drawButton(ren,ct.x+366,ct.y+21,92,16,"APPEND PLAYER",false);
        cart::drawText(ren, ruleColumn==cart::RuleColumn::Group?"RULES *":"RULES", x0, ct.y + 46, 1, 150, 200, 150);
        cart::drawText(ren, ruleColumn==cart::RuleColumn::Condition?"CONDITIONS *":"CONDITIONS", x1, ct.y + 46, 1, 150, 200, 150);
        cart::drawText(ren, ruleColumn==cart::RuleColumn::Action?"ACTIONS *":"ACTIONS", x2, ct.y + 46, 1, 150, 200, 150);
        int listY = ct.y + 58, listH = ct.h - 58 - 138;
        rRuleList = {x0, listY, colW, listH};
        rCondList = {x1, listY, colW, listH};
        rActList = {x2, listY, colW, listH};
        for (const SDL_Rect* a : {&rRuleList, &rCondList, &rActList}) {
            SDL_SetRenderDrawColor(ren, 22, 24, 32, 255); SDL_RenderFillRect(ren, a);
            SDL_SetRenderDrawColor(ren, 70, 74, 90, 255); SDL_RenderDrawRect(ren, a);
        }
        // Rule (group) list.
        SDL_RenderSetClipRect(ren, &rRuleList);
        for (int i = 0; i < int(gs.size()); ++i) {
            int ry = listY + i * kRow - scrRuleScroll;
            if (ry + kRow < listY || ry > listY + listH) continue;
            bool sel = i == scrGroup;
            if (sel) { SDL_SetRenderDrawColor(ren, 58, 68, 95, 255); SDL_Rect hr{x0, ry, colW, kRow}; SDL_RenderFillRect(ren, &hr); }
            std::string lbl = (gs[size_t(i)].editorName.empty()?"Rule " + std::to_string(i + 1):gs[size_t(i)].editorName) + "  " +
                std::to_string(gs[size_t(i)].conditions.size()) + "c/" +
                std::to_string(gs[size_t(i)].actions.size()) + "a";
            cart::drawText(ren, lbl, x0 + 3, ry + 2, 1, sel ? 255 : 200, sel ? 235 : 205, sel ? 200 : 215);
        }
        SDL_RenderSetClipRect(ren, nullptr);
        // Conditions + actions of the selected group.
        auto drawRules = [&](const SDL_Rect& area, int scroll, const std::vector<tak::crt::Rule>* rules,
                             bool isAct, int selRow) {
            if (!rules) return;
            SDL_RenderSetClipRect(ren, &area);
            for (int i = 0; i < int(rules->size()); ++i) {
                int ry = area.y + i * kRow - scroll;
                if (ry + kRow < area.y || ry > area.y + area.h) continue;
                bool sel = i == selRow;
                if (sel) { SDL_SetRenderDrawColor(ren, 58, 68, 95, 255); SDL_Rect hr{area.x, ry, area.w, kRow}; SDL_RenderFillRect(ren, &hr); }
                cart::drawText(ren, cart::formatRule(isAct, (*rules)[size_t(i)]), area.x + 3, ry + 2, 1,
                               sel ? 255 : 205, sel ? 235 : 210, sel ? 200 : 220);
            }
            SDL_RenderSetClipRect(ren, nullptr);
        };
        drawRules(rCondList, scrCondScroll, g ? &g->conditions : nullptr, false, scrCondSel);
        drawRules(rActList, scrActScroll, g ? &g->actions : nullptr, true, scrActSel);
        const auto detail=g?cart::ruleDetails(*g,ruleColumn,ruleColumn==cart::RuleColumn::Action?scrActSel:scrCondSel):std::string("Select a rule to read its conditions and actions.");
        if(detail!=scrDetailText) {scrDetailText=detail;scrDetailScroll=0;}
        const int detailY=listY+listH+8;
        cart::drawText(ren,"FULL TEXT (PgUp/PgDn or mouse wheel)",ct.x,detailY,1,180,205,225);
        rCopyRuleText=cart::drawButton(ren,ct.x+ct.w-76,detailY-3,76,14,"COPY TEXT",false);
        rRuleDetail={ct.x,detailY+13,ct.w,78};
        SDL_SetRenderDrawColor(ren,18,21,29,255);SDL_RenderFillRect(ren,&rRuleDetail);
        const auto rows=cart::TextEdit::lines(scrDetailText,std::max(1,(rRuleDetail.w-12)/6));
        const int visible=(rRuleDetail.h-8)/12;
        scrDetailMax=std::max(0,int(rows.size())-visible);scrDetailScroll=std::clamp(scrDetailScroll,0,scrDetailMax);
        SDL_RenderSetClipRect(ren,&rRuleDetail);
        for(int row=scrDetailScroll;row<std::min(int(rows.size()),scrDetailScroll+visible);++row) {
            const auto& line=rows[row];
            cart::drawText(ren,scrDetailText.substr(line.begin,line.end-line.begin),rRuleDetail.x+4,rRuleDetail.y+4+(row-scrDetailScroll)*12,1,230,235,245);
        }
        SDL_RenderSetClipRect(ren,nullptr);
        // Column action buttons.
        int by = ct.y + ct.h - 18;
        rAddRule = cart::drawButton(ren, x0, by, 44, 16, "+RULE", true);
        rDelRule = cart::drawButton(ren, x0 + 48, by, 44, 16, "-RULE", false);
        rNameRule=cart::drawButton(ren,x0+96,by,44,16,"NAME",false);
        rAddCond = cart::drawButton(ren, x1, by, 44, 16, "+COND", true);
        rDelCond = cart::drawButton(ren, x1 + 48, by, 44, 16, "-COND", false);
        rAddAct = cart::drawButton(ren, x2, by, 40, 16, "+ACT", true);
        rDelAct = cart::drawButton(ren, x2 + 44, by, 40, 16, "-ACT", false);
        cart::drawText(ren, "Ctrl+C/V/D: copy/paste/duplicate. Alt+Up/Down: reorder selected column.",
                       ct.x, by - 12, 1, 150, 154, 168);

        // Nested opcode picker (choose a condition/action type to add).
        if(pickOpen && pickTemplate) {
            const auto pc=cart::drawPanel(ren,w,h,580,420,"OBJECTIVE TEMPLATES");
            rTemplateSearch=cart::drawField(ren,pc.x,pc.y,pc.w,"SEARCH (UP/DOWN THEN ENTER, OR CLICK A TEMPLATE)",templateQuery,true,&templateEditor);
            rPickList={pc.x,pc.y+36,pc.w,252};
            const auto matches=cart::matchingRuleTemplates(templateCatalog,templateQuery);
            SDL_RenderSetClipRect(ren,&rPickList);
            for(int i=0;i<int(matches.size());++i) {
                const auto& item=templateCatalog[matches[i]];const int y=pc.y+36+i*42;
                if(i==templateSelection) {SDL_SetRenderDrawColor(ren,50,65,90,255);SDL_Rect row{pc.x,y,pc.w,40};SDL_RenderFillRect(ren,&row);}
                cart::drawText(ren,item.name,pc.x+4,y+5,1,235,225,180);
                cart::drawText(ren,item.description,pc.x+4,y+20,1,200,215,235);
            }
            if(matches.empty())cart::drawText(ren,"No matching templates.",pc.x+4,pc.y+42,1,215,220,235);
            SDL_RenderSetClipRect(ren,nullptr);
            cart::drawText(ren,"Inserted rules are editable. Check their unit, area and time.",pc.x,pc.y+300,1,190,205,220);
            cart::drawText(ren,"Area: selected/first region, or Anywhere when none exist.",pc.x,pc.y+314,1,190,205,220);
            rPickCancel=cart::drawButton(ren,pc.x+pc.w-74,pc.y+pc.h-20,70,18,"CANCEL",false);
        } else if (pickOpen) {
            const auto& defs = pickAction ? cart::actionDefs() : cart::conditionDefs();
            SDL_Rect pc = cart::drawPanel(ren, w, h, 480, 400, pickAction ? "ADD ACTION" : "ADD CONDITION");
            rPickList = {pc.x, pc.y, pc.w, pc.h - 28};
            SDL_RenderSetClipRect(ren, &rPickList);
            for (int i = 0; i < int(defs.size()); ++i) {
                int ry = pc.y + i * kRow - pickScroll;
                if (ry + kRow < pc.y || ry > pc.y + rPickList.h) continue;
                cart::drawText(ren, defs[size_t(i)].templ, pc.x + 3, ry + 2, 1, 210, 214, 225);
            }
            SDL_RenderSetClipRect(ren, nullptr);
            rPickCancel = cart::drawButton(ren, pc.x + pc.w - 74, pc.y + pc.h - 20, 70, 18, "CANCEL", false);
        }
    }
}
