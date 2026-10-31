// Portions Copyright 2026 The Hollycast Authors
// Devanagari shaping for ImGui's existing FreeType font loader.
#pragma once
#include "imgui.h"

#if defined(IMGUI_ENABLE_HARFBUZZ) && !defined(IMGUI_DISABLE)
struct ImGuiShapedGlyph
{
    unsigned int GlyphId;
    unsigned int Cluster;
    float Advance;
    ImVec2 Offset;
    bool UnsafeToBreak;
};

// Keep glyph IDs inside the font atlas. Application strings, IDs, clipboard text
// and formatting arguments remain ordinary Unicode throughout the UI.
namespace ImGuiFreeType
{
IMGUI_API bool ShapeRun(ImFontBaked* baked, const char* begin, const char* end, ImFontConfig** source, ImVector<ImGuiShapedGlyph>* glyphs);
IMGUI_API const ImFontGlyph* LoadGlyphIndex(ImFontBaked* baked, ImFontConfig* source, unsigned int glyph_id);
}

class ImGuiTextShaper
{
public:
    struct Cluster
    {
        const char* Begin;
        const char* End;
        int FirstGlyph;
        int GlyphCount;
        float Advance;
    };

    explicit ImGuiTextShaper(ImFontBaked* baked) : Baked(baked) {}
    const Cluster* Next(const char* begin, const char* end);

    static bool NeedsShaping(const char* begin, const char* end);
    static const char* ClusterBoundary(ImFont* font, float size, const char* begin, const char* end, const char* position, bool round_up);
    static void RenderText(ImFont* font, ImDrawList* draw_list, float size, const ImVec2& pos, ImU32 col, const ImVec4& clip_rect, const char* begin, const char* end, float wrap_width, int flags);

private:
    void Draw(ImDrawList* draw_list, const Cluster& cluster, const ImVec2& pos, float scale, ImU32 col, const ImVec4& clip_rect);
    ImFontBaked* Baked;
    ImFontConfig* Source = nullptr;
    ImVector<ImGuiShapedGlyph> Glyphs;
    ImVector<Cluster> Clusters;
    int CurrentCluster = 0;
};
#endif
