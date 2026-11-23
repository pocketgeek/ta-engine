#pragma once

// Minimal immediate-mode widgets for Cartographer's modal dialogs, drawn with
// the 5x7 bitmap font. The modal state + apply logic lives in the editor; these
// are just draw + hit-test helpers so each dialog stays a few lines.

#include "cartographer/font5x7.h"
#include "cartographer/textedit.h"

#include <SDL.h>

#include <string>

namespace cart {

inline bool pointIn(int mx, int my, const SDL_Rect& r) {
    return mx >= r.x && my >= r.y && mx < r.x + r.w && my < r.y + r.h;
}

// A labelled text box. Draws the label above and the (optionally focused) value
// box; returns the value box rect so the caller can hit-test focus clicks.
inline SDL_Rect drawField(SDL_Renderer* ren, int x, int y, int w,
                          const std::string& label, const std::string& text,
                          bool focused, const TextEdit* edit = nullptr) {
    drawText(ren, label, x, y, 1, 190, 195, 205);
    SDL_Rect box{x, y + 10, w, 16};
    SDL_SetRenderDrawColor(ren, focused ? 40 : 24, focused ? 44 : 26, focused ? 60 : 34, 255);
    SDL_RenderFillRect(ren, &box);
    SDL_SetRenderDrawColor(ren, focused ? 255 : 90, focused ? 210 : 92, focused ? 90 : 104, 255);
    SDL_RenderDrawRect(ren, &box);
    // Clip the text to the box; show the tail (caret end) when it overflows.
    SDL_RenderSetClipRect(ren, &box);
    int tw = textWidth(text, 1);
    const int caretWidth=edit?textWidth(text.substr(0,edit->caret),1):tw;
    int tx = caretWidth + 6 <= w ? x + 4 : x + 4 - (caretWidth + 6 - w);
    if(focused && edit && edit->selection()) {
        const auto a=std::min(edit->caret,edit->anchor),b=std::max(edit->caret,edit->anchor);
        SDL_SetRenderDrawColor(ren,65,85,130,255);
        SDL_Rect selected{tx+textWidth(text.substr(0,a),1),y+12,textWidth(text.substr(a,b-a),1),11};
        SDL_RenderFillRect(ren,&selected);
    }
    drawText(ren, text, tx, y + 14, 1, 230, 235, 245);
    if (focused) {   // caret
        int cx = tx + caretWidth + 1;
        SDL_SetRenderDrawColor(ren, 255, 220, 120, 255);
        SDL_Rect c{cx, y + 13, 1, 9};
        SDL_RenderFillRect(ren, &c);
    }
    SDL_RenderSetClipRect(ren, nullptr);
    return box;
}

// Word-wrapped multiline field. Offsets stay in UTF-8 bytes; wrapping and clicks
// use code-point columns so they never split a stored character.
inline SDL_Rect drawTextArea(SDL_Renderer* ren,int x,int y,int w,int height,
    const std::string& label,const std::string& text,bool focused,const TextEdit& edit,
    int& scroll,bool followCaret) {
    drawText(ren,label,x,y,1,190,195,205);
    SDL_Rect box{x,y+10,w,height};const int columns=std::max(1,(w-8)/6),visible=std::max(1,(height-8)/12);
    const auto rows=TextEdit::lines(text,columns);const auto caretRow=TextEdit::lineAt(rows,edit.caret);
    if(focused && followCaret) {if(int(caretRow)<scroll)scroll=int(caretRow);if(int(caretRow)>=scroll+visible)scroll=int(caretRow)-visible+1;}
    scroll=std::clamp(scroll,0,std::max(0,int(rows.size())-visible));
    SDL_SetRenderDrawColor(ren,24,26,34,255);SDL_RenderFillRect(ren,&box);
    SDL_SetRenderDrawColor(ren,focused?255:90,focused?210:92,focused?90:104,255);SDL_RenderDrawRect(ren,&box);
    SDL_RenderSetClipRect(ren,&box);
    for(int i=scroll;i<std::min(int(rows.size()),scroll+visible);++i) {
        const auto& row=rows[i];const int top=box.y+4+(i-scroll)*12;
        const auto first=std::max(row.begin,std::min(edit.caret,edit.anchor)),last=std::min(row.end,std::max(edit.caret,edit.anchor));
        if(focused && first<last) {
            SDL_SetRenderDrawColor(ren,65,85,130,255);
            SDL_Rect selection{x+4+textWidth(text.substr(row.begin,first-row.begin),1),top-1,textWidth(text.substr(first,last-first),1),10};SDL_RenderFillRect(ren,&selection);
        }
        drawText(ren,text.substr(row.begin,row.end-row.begin),x+4,top,1,230,235,245);
        if(focused && i==int(caretRow)) {
            const int cx=x+4+textWidth(text.substr(row.begin,edit.caret-row.begin),1);
            SDL_SetRenderDrawColor(ren,255,220,120,255);SDL_RenderDrawLine(ren,cx,top-1,cx,top+8);
        }
    }
    SDL_RenderSetClipRect(ren,nullptr);return box;
}

// A labelled dropdown box: draws the label above and a value box with a "v"
// marker on the right. `open` highlights it (its list is showing). Returns the
// box rect so the caller can hit-test the click that opens/closes the list.
inline SDL_Rect drawChoice(SDL_Renderer* ren, int x, int y, int w,
                           const std::string& label, const std::string& value,
                           bool open) {
    drawText(ren, label, x, y, 1, 190, 195, 205);
    SDL_Rect box{x, y + 10, w, 16};
    SDL_SetRenderDrawColor(ren, open ? 40 : 24, open ? 44 : 26, open ? 60 : 34, 255);
    SDL_RenderFillRect(ren, &box);
    SDL_SetRenderDrawColor(ren, open ? 255 : 90, open ? 210 : 92, open ? 90 : 104, 255);
    SDL_RenderDrawRect(ren, &box);
    drawText(ren, value, x + 4, y + 14, 1, 230, 235, 245);
    // Dropdown marker: a filled down-pointing triangle at the right edge, drawn as
    // stacked narrowing rows (SDL has no filled-triangle primitive).
    SDL_SetRenderDrawColor(ren, 210, 215, 225, 255);
    int ax = x + w - 12, ay = y + 15;
    for (int r = 0; r < 4; ++r) {
        SDL_Rect row{ax + r, ay + r, 8 - 2 * r, 1};
        SDL_RenderFillRect(ren, &row);
    }
    return box;
}

// A button; draws it and returns its rect (caller hit-tests clicks).
inline SDL_Rect drawButton(SDL_Renderer* ren, int x, int y, int w, int h,
                           const std::string& label, bool primary) {
    SDL_Rect r{x, y, w, h};
    SDL_SetRenderDrawColor(ren, primary ? 70 : 52, primary ? 90 : 54, primary ? 60 : 66, 255);
    SDL_RenderFillRect(ren, &r);
    SDL_SetRenderDrawColor(ren, primary ? 150 : 96, primary ? 200 : 98, primary ? 120 : 112, 255);
    SDL_RenderDrawRect(ren, &r);
    int lw = textWidth(label, 1);
    drawText(ren, label, x + (w - lw) / 2, y + (h - 7) / 2, 1,
             primary ? 255 : 210, primary ? 235 : 210, primary ? 200 : 220);
    return r;
}

// A modal panel: dim the screen, draw a titled box centred at (cx,cy). Returns
// the panel's content origin (top-left inside the border, below the title).
// Wrap hover help and keep it inside the logical window, including at high DPI.
inline void drawTooltip(SDL_Renderer* ren,int windowWidth,int windowHeight,int x,int y,const std::string& text) {
    const int width=std::max(48,std::min(360,windowWidth-12));
    const auto lines=TextEdit::lines(text,std::max(1,(width-12)/6));
    const int height=int(lines.size())*12+12;
    SDL_Rect box{std::clamp(x+12,6,std::max(6,windowWidth-width-6)),
                 y+22+height<=windowHeight-6?y+22:std::max(6,y-height-8),width,height};
    SDL_SetRenderDrawColor(ren,20,25,34,245);SDL_RenderFillRect(ren,&box);
    SDL_SetRenderDrawColor(ren,155,170,190,255);SDL_RenderDrawRect(ren,&box);
    SDL_RenderSetClipRect(ren,&box);
    for(size_t i=0;i<lines.size();++i)drawText(ren,text.substr(lines[i].begin,lines[i].end-lines[i].begin),box.x+6,box.y+6+int(i)*12,1,235,240,250);
    SDL_RenderSetClipRect(ren,nullptr);
}

inline SDL_Rect drawPanel(SDL_Renderer* ren, int winW, int winH, int pw, int ph,
                          const std::string& title) {
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 150);
    SDL_Rect dim{0, 0, winW, winH};
    SDL_RenderFillRect(ren, &dim);
    SDL_Rect p{(winW - pw) / 2, (winH - ph) / 2, pw, ph};
    SDL_SetRenderDrawColor(ren, 40, 42, 52, 255);
    SDL_RenderFillRect(ren, &p);
    SDL_SetRenderDrawColor(ren, 120, 124, 140, 255);
    SDL_RenderDrawRect(ren, &p);
    SDL_Rect tb{p.x, p.y, p.w, 20};
    SDL_SetRenderDrawColor(ren, 60, 64, 80, 255);
    SDL_RenderFillRect(ren, &tb);
    SDL_RenderSetClipRect(ren,&tb);
    drawText(ren, title, p.x + 8, p.y + 7, 1, 235, 235, 245);
    SDL_RenderSetClipRect(ren,nullptr);
    return SDL_Rect{p.x + 14, p.y + 30, p.w - 28, p.h - 40};
}

} // namespace cart
