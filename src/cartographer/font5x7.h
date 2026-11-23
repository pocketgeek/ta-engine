#pragma once

// A tiny self-contained 5x7 bitmap font for the editor chrome (status bar, tool
// labels, start-position numbers). No external assets: each glyph is 7 rows of 5
// bits (bit 4 = leftmost). Covers the characters the UI needs; unknown chars draw
// a visible replacement glyph. Basic Latin glyph bitmaps are functional utility data, not a typeface.

#include <SDL.h>
#include "cartographer/editorfont.h"
#include "cartographer/textwidth.h"

#include <cstdint>
#include <string>
#include <vector>

namespace cart {

inline const uint8_t* glyph5x7(char c) {
    // 7 rows, low 5 bits each. Authored for: space ! % ( ) , - . / 0-9 : A-Z x.
    static const uint8_t SP[7]  = {0,0,0,0,0,0,0};
    static const uint8_t UNKNOWN[7]={31,17,21,21,21,17,31};
    static const uint8_t LB[7]={14,8,8,8,8,8,14},RB[7]={14,2,2,2,2,2,14};
    static const uint8_t PLUS[7]={0,4,4,31,4,4,0},EQUAL[7]={0,0,31,0,31,0,0};
    static const uint8_t UNDER[7]={0,0,0,0,0,0,31},STAR[7]={0,21,14,31,14,21,0};
    static const uint8_t QUOTE[7]={10,10,0,0,0,0,0},APOST[7]={4,4,0,0,0,0,0};
    static const uint8_t QUESTION[7]={14,17,1,2,4,0,4},SEMI[7]={0,4,4,0,4,4,8};
    static const uint8_t LESS[7]={2,4,8,16,8,4,2},GREATER[7]={8,4,2,1,2,4,8};
    static const uint8_t HASH[7]={10,10,31,10,31,10,10},BACK[7]={16,16,8,4,2,1,1};
    static const uint8_t BANG[7]= {0x04,0x04,0x04,0x04,0x04,0x00,0x04};
    static const uint8_t PCT[7] = {0x19,0x1A,0x02,0x04,0x08,0x0B,0x13};
    static const uint8_t LP[7]  = {0x02,0x04,0x08,0x08,0x08,0x04,0x02};
    static const uint8_t RP[7]  = {0x08,0x04,0x02,0x02,0x02,0x04,0x08};
    static const uint8_t COMMA[7]={0,0,0,0,0x04,0x04,0x08};
    static const uint8_t DASH[7]= {0,0,0,0x0E,0,0,0};
    static const uint8_t DOT[7] = {0,0,0,0,0,0x0C,0x0C};
    static const uint8_t SL[7]  = {0x01,0x01,0x02,0x04,0x08,0x10,0x10};
    static const uint8_t COLON[7]={0,0x0C,0x0C,0,0x0C,0x0C,0};
    static const uint8_t D0[7]={0x0E,0x11,0x13,0x15,0x19,0x11,0x0E};
    static const uint8_t D1[7]={0x04,0x0C,0x04,0x04,0x04,0x04,0x0E};
    static const uint8_t D2[7]={0x0E,0x11,0x01,0x02,0x04,0x08,0x1F};
    static const uint8_t D3[7]={0x1F,0x02,0x04,0x02,0x01,0x11,0x0E};
    static const uint8_t D4[7]={0x02,0x06,0x0A,0x12,0x1F,0x02,0x02};
    static const uint8_t D5[7]={0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E};
    static const uint8_t D6[7]={0x06,0x08,0x10,0x1E,0x11,0x11,0x0E};
    static const uint8_t D7[7]={0x1F,0x01,0x02,0x04,0x08,0x08,0x08};
    static const uint8_t D8[7]={0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E};
    static const uint8_t D9[7]={0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C};
    static const uint8_t A[7]={0x0E,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t B[7]={0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E};
    static const uint8_t C[7]={0x0E,0x11,0x10,0x10,0x10,0x11,0x0E};
    static const uint8_t D[7]={0x1C,0x12,0x11,0x11,0x11,0x12,0x1C};
    static const uint8_t E[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F};
    static const uint8_t F[7]={0x1F,0x10,0x10,0x1E,0x10,0x10,0x10};
    static const uint8_t G[7]={0x0E,0x11,0x10,0x17,0x11,0x11,0x0F};
    static const uint8_t H[7]={0x11,0x11,0x11,0x1F,0x11,0x11,0x11};
    static const uint8_t I[7]={0x0E,0x04,0x04,0x04,0x04,0x04,0x0E};
    static const uint8_t J[7]={0x07,0x02,0x02,0x02,0x02,0x12,0x0C};
    static const uint8_t K[7]={0x11,0x12,0x14,0x18,0x14,0x12,0x11};
    static const uint8_t L[7]={0x10,0x10,0x10,0x10,0x10,0x10,0x1F};
    static const uint8_t M[7]={0x11,0x1B,0x15,0x15,0x11,0x11,0x11};
    static const uint8_t N[7]={0x11,0x19,0x15,0x13,0x11,0x11,0x11};
    static const uint8_t O[7]={0x0E,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t P[7]={0x1E,0x11,0x11,0x1E,0x10,0x10,0x10};
    static const uint8_t Q[7]={0x0E,0x11,0x11,0x11,0x15,0x12,0x0D};
    static const uint8_t R[7]={0x1E,0x11,0x11,0x1E,0x14,0x12,0x11};
    static const uint8_t S[7]={0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E};
    static const uint8_t T[7]={0x1F,0x04,0x04,0x04,0x04,0x04,0x04};
    static const uint8_t U[7]={0x11,0x11,0x11,0x11,0x11,0x11,0x0E};
    static const uint8_t V[7]={0x11,0x11,0x11,0x11,0x11,0x0A,0x04};
    static const uint8_t W[7]={0x11,0x11,0x11,0x15,0x15,0x1B,0x11};
    static const uint8_t X[7]={0x11,0x11,0x0A,0x04,0x0A,0x11,0x11};
    static const uint8_t Y[7]={0x11,0x11,0x0A,0x04,0x04,0x04,0x04};
    static const uint8_t Z[7]={0x1F,0x01,0x02,0x04,0x08,0x10,0x1F};
    switch (c) {
        case '[':return LB;case ']':return RB;case '+':return PLUS;case '=':return EQUAL;
        case '_':return UNDER;case '*':return STAR;case '"':return QUOTE;case 39:return APOST;
        case '?':return QUESTION;case ';':return SEMI;case '<':return LESS;case '>':return GREATER;
        case '#':return HASH;case 92:return BACK;
        case ' ': return SP;  case '!': return BANG; case '%': return PCT;
        case '(': return LP;  case ')': return RP;   case ',': return COMMA;
        case '-': return DASH; case '.': return DOT; case '/': return SL;
        case ':': return COLON;
        case '0': return D0; case '1': return D1; case '2': return D2;
        case '3': return D3; case '4': return D4; case '5': return D5;
        case '6': return D6; case '7': return D7; case '8': return D8;
        case '9': return D9;
        case 'x': return X;   // lowercase x -> X glyph ("6 x 6")
        default: break;
    }
    if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    switch (c) {
        case 'A':return A; case 'B':return B; case 'C':return C; case 'D':return D;
        case 'E':return E; case 'F':return F; case 'G':return G; case 'H':return H;
        case 'I':return I; case 'J':return J; case 'K':return K; case 'L':return L;
        case 'M':return M; case 'N':return N; case 'O':return O; case 'P':return P;
        case 'Q':return Q; case 'R':return R; case 'S':return S; case 'T':return T;
        case 'U':return U; case 'V':return V; case 'W':return W; case 'X':return X;
        case 'Y':return Y; case 'Z':return Z;
        default: return UNKNOWN;
    }
}

// Decode one code point without treating UTF-8 continuation bytes as glyphs.
inline uint32_t nextGlyph(const std::string& text,size_t& position) {
    const auto first=uint8_t(text[position++]);if(first<128)return first;
    int count=first>=0xc2 && first<=0xdf?1:first>=0xe0 && first<=0xef?2:first>=0xf0 && first<=0xf4?3:0;
    if(!count || position+count>text.size())return 0xfffd;
    uint32_t code=first&((1u<<(6-count))-1);
    for(int i=0;i<count;++i) {const auto byte=uint8_t(text[position+i]);if((byte&0xc0)!=0x80)return 0xfffd;code=(code<<6)|(byte&63);}
    position+=count;
    if((count==2 && code<0x800) || (count==3 && code<0x10000) || (code>=0xd800 && code<=0xdfff) || code>0x10ffff)return 0xfffd;
    return code;
}

// Draw `text` at (x,y) scaled by `s`, colour (r,g,b). One batched FillRects call.
inline void drawText(SDL_Renderer* ren, const std::string& text, int x, int y, int s,
                     Uint8 r, Uint8 g, Uint8 b) {
    if(drawEditorText(ren,text,x,y,s,r,g,b))return;
    std::vector<SDL_Rect> px;
    int cx = x;
    for(size_t position=0;position<text.size();) {
        const auto code=nextGlyph(text,position);
        const uint8_t* gp = glyph5x7(code<128?char(code):char(127));
        for (int row = 0; row < 7; ++row)
            for (int col = 0; col < 5; ++col)
                if (gp[row] & (1 << (4 - col)))
                    px.push_back({cx + col * s, y + row * s, s, s});
        cx += 6 * s * glyphCells(code);   // 5px glyph + 1px gap per column
    }
    if (px.empty()) return;
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    SDL_RenderFillRects(ren, px.data(), int(px.size()));
}

// Width in text columns (a wide glyph is two) and in pixels at scale `s`.
inline int textColumns(const std::string& t) {int count=0;for(size_t p=0;p<t.size();)count+=glyphCells(nextGlyph(t,p));return count;}
inline int textWidth(const std::string& t,int s) {return textColumns(t)*6*s;}
// The longest prefix of `t` that fits `columns` text columns, cut at a code point boundary.
inline std::string clipColumns(const std::string& t,int columns) {
    int used=0;size_t p=0;
    while(p<t.size()) {
        const size_t before=p;const int cells=glyphCells(nextGlyph(t,p));
        if(used+cells>columns)return t.substr(0,before);
        used+=cells;
    }
    return t;
}

} // namespace cart
