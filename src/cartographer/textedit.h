#pragma once
#include "cartographer/font5x7.h"
#include <SDL.h>
#include <algorithm>
#include <string>
#include <vector>

namespace cart {
// Positions are UTF-8 byte offsets at code-point boundaries. All storage and
// clipboard operations preserve Unicode even if a chosen UI font lacks a glyph.
// "Columns" are text columns of the fixed-width editor font: a wide (CJK etc.)
// code point takes two (glyphCells), so wrapping, clicks and Up/Down follow the pixels.
struct TextEdit {
    size_t caret=0,anchor=0;
    int desiredColumn=-1;
    struct Line {size_t begin,end;};
    static size_t previous(const std::string& s,size_t p) {
        if(!p)return 0;
        --p;while(p && (uint8_t(s[p])&0xc0)==0x80)--p;return p;
    }
    static size_t next(const std::string& s,size_t p) {
        if(p>=s.size())return s.size();
        ++p;while(p<s.size() && (uint8_t(s[p])&0xc0)==0x80)++p;return p;
    }
    static int cellsAt(const std::string& s,size_t p) {return glyphCells(nextGlyph(s,p));}   // same decoder as drawing
    static std::vector<Line> lines(const std::string& text,int columns) {
        std::vector<Line> result;size_t begin=0,pos=0,word=0;int count=0;
        while(pos<text.size()) {
            if(text[pos]=='\n') {result.push_back({begin,pos});begin=++pos;word=begin;count=0;continue;}
            const int cells=cellsAt(text,pos);
            // A wide glyph that does not fit moves to the next row; a row always takes at least one glyph.
            if(columns>0 && count>0 && count+cells>columns) {
                const auto end=word>begin?word:pos;result.push_back({begin,end});begin=pos=end;word=begin;count=0;continue;
            }
            const bool space=text[pos]==' ';pos=next(text,pos);count+=cells;if(space)word=pos;
        }
        result.push_back({begin,text.size()});return result;
    }
    static size_t lineAt(const std::vector<Line>& rows,size_t offset) {
        for(size_t i=0;i<rows.size();++i)if(offset<=rows[i].end && (i+1==rows.size() || offset<rows[i+1].begin))return i;
        return rows.size()-1;
    }
    // Caret offset for a clicked text column: before the glyph under the pointer, or after a wide
    // glyph when its right half was hit. Also maps an Up/Down column back to an offset.
    static size_t offsetAt(const std::string& text,const Line& line,int column) {
        size_t at=line.begin;int col=0;
        while(at<line.end && col<column) {col+=cellsAt(text,at);at=next(text,at);}
        return at;
    }
    static int columnOf(const std::string& text,const Line& line,size_t offset) {
        int col=0;for(size_t at=line.begin;at<offset && at<line.end;at=next(text,at))col+=cellsAt(text,at);return col;
    }
    void click(const std::string& text,int columns,int row,int column,bool extend=false) {
        const auto rows=lines(text,columns);row=std::clamp(row,0,int(rows.size())-1);
        caret=offsetAt(text,rows[row],std::max(0,column));if(!extend)anchor=caret;desiredColumn=-1;
    }
    void focus(const std::string& s,bool select=true) {caret=s.size();anchor=select?0:caret;desiredColumn=-1;}
    bool selection() const {return caret!=anchor;}
    void erase(std::string& s) {const auto first=std::min(caret,anchor);s.erase(first,std::max(caret,anchor)-first);caret=anchor=first;}
    bool input(const SDL_Event& e,std::string& s,bool numeric=false,bool multiline=false,int columns=0) {
        caret=std::min(caret,s.size());anchor=std::min(anchor,s.size());
        const auto original=s;
        auto insert=[&](std::string text) {
            text.erase(std::remove_if(text.begin(),text.end(),[&](unsigned char c) {
                return numeric ? !(c>='0' && c<='9') && c!='-' : c<32 && !(multiline && c=='\n');
            }),text.end());
            if(s.size()-(std::max(caret,anchor)-std::min(caret,anchor))+text.size()>65536)return;
            erase(s);s.insert(caret,text);caret+=text.size();anchor=caret;
        };
        if(e.type==SDL_TEXTINPUT) {desiredColumn=-1;insert(e.text.text);}
        else if(e.type==SDL_KEYDOWN) {
            const auto key=e.key.keysym.sym;const bool ctrl=e.key.keysym.mod&(KMOD_CTRL|KMOD_GUI),shift=e.key.keysym.mod&KMOD_SHIFT;
            if(key!=SDLK_UP && key!=SDLK_DOWN)desiredColumn=-1;
            if(ctrl && key==SDLK_a) {anchor=0;caret=s.size();}
            else if(ctrl && (key==SDLK_c || key==SDLK_x)) {
                if(selection()) {SDL_SetClipboardText(s.substr(std::min(caret,anchor),std::max(caret,anchor)-std::min(caret,anchor)).c_str());if(key==SDLK_x)erase(s);}
            } else if(ctrl && key==SDLK_v) {if(char* text=SDL_GetClipboardText()) {insert(text);SDL_free(text);}}
            else if(key==SDLK_BACKSPACE) {if(!selection())anchor=previous(s,caret);erase(s);}
            else if(key==SDLK_DELETE) {if(!selection())anchor=next(s,caret);erase(s);}
            else if(multiline && (key==SDLK_UP || key==SDLK_DOWN)) {
                const auto rows=lines(s,columns);const auto row=lineAt(rows,caret);
                if(desiredColumn<0)desiredColumn=columnOf(s,rows[row],caret);
                const int target=std::clamp(int(row)+(key==SDLK_UP?-1:1),0,int(rows.size())-1);
                caret=offsetAt(s,rows[target],desiredColumn);if(!shift)anchor=caret;
            } else if(key==SDLK_LEFT || key==SDLK_RIGHT || key==SDLK_HOME || key==SDLK_END) {
                if(key==SDLK_HOME || key==SDLK_END) {
                    if(multiline && !ctrl) {const auto rows=lines(s,columns);const auto& row=rows[lineAt(rows,caret)];caret=key==SDLK_HOME?row.begin:row.end;}
                    else caret=key==SDLK_HOME?0:s.size();
                }
                else if(selection() && !shift)caret=key==SDLK_LEFT?std::min(caret,anchor):std::max(caret,anchor);
                else caret=key==SDLK_LEFT?previous(s,caret):next(s,caret);
                if(!shift)anchor=caret;
            } else if(multiline && key==SDLK_RETURN && shift)insert("\n");
        }
        return s!=original;
    }
};
} // namespace cart
