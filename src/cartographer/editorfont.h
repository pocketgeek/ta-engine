#pragma once
#include <SDL.h>
#include <string>
namespace cart {
// Embedded glyph pages; no runtime font dependency. Must die before renderer.
class EditorFont {
public:
    explicit EditorFont(SDL_Renderer* renderer);
    ~EditorFont();
    EditorFont(const EditorFont&)=delete;
    EditorFont& operator=(const EditorFont&)=delete;
private:
    SDL_Renderer* renderer_;
};
bool drawEditorText(SDL_Renderer*,const std::string&,int x,int y,int scale,Uint8 r,Uint8 g,Uint8 b);
}
