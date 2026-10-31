// Portions Copyright 2026 The Hollycast Authors
#include "imgui_text_shaping.h"

#if defined(IMGUI_ENABLE_HARFBUZZ) && !defined(IMGUI_DISABLE)
#include "imgui_internal.h"

namespace
{
bool IsDevanagari(unsigned int c)
{
    return (c >= 0x0900 && c <= 0x097f) || (c >= 0x1cd0 && c <= 0x1cff) || (c >= 0xa8e0 && c <= 0xa8ff);
}

// Glyphs may be inserted, combined or reordered by OpenType substitutions. Draw
// their actual atlas entries and HarfBuzz offsets rather than Unicode stand-ins.
void DrawGlyph(ImDrawList* draw_list, const ImFontGlyph* glyph, const ImVec2& pos, float scale, ImU32 col, const ImVec4& clip_rect)
{
    if (!glyph || !glyph->Visible)
        return;
    ImVec2 min(pos.x + glyph->X0 * scale, pos.y + glyph->Y0 * scale);
    ImVec2 max(pos.x + glyph->X1 * scale, pos.y + glyph->Y1 * scale);
    if (min.x >= clip_rect.z || max.x <= clip_rect.x || min.y >= clip_rect.w || max.y <= clip_rect.y)
        return;
    ImVec2 uv_min(glyph->U0, glyph->V0);
    ImVec2 uv_max(glyph->U1, glyph->V1);
    const ImVec2 uv_scale((uv_max.x - uv_min.x) / (max.x - min.x), (uv_max.y - uv_min.y) / (max.y - min.y));
    if (min.x < clip_rect.x) { uv_min.x += (clip_rect.x - min.x) * uv_scale.x; min.x = clip_rect.x; }
    if (min.y < clip_rect.y) { uv_min.y += (clip_rect.y - min.y) * uv_scale.y; min.y = clip_rect.y; }
    if (max.x > clip_rect.z) { uv_max.x -= (max.x - clip_rect.z) * uv_scale.x; max.x = clip_rect.z; }
    if (max.y > clip_rect.w) { uv_max.y -= (max.y - clip_rect.w) * uv_scale.y; max.y = clip_rect.w; }
    draw_list->PrimReserve(6, 4);
    draw_list->PrimRectUV(min, max, uv_min, uv_max, glyph->Colored ? col | ~IM_COL32_A_MASK : col);
}
}

bool ImGuiTextShaper::NeedsShaping(const char* begin, const char* end)
{
    for (const char* s = begin; s < end;)
    {
        if ((unsigned char)*s < 0x80)
        {
            ++s;
            continue;
        }
        unsigned int c;
        const int length = ImTextCharFromUtf8(&c, s, end);
        if (length == 0)
            break;
        if (IsDevanagari(c))
            return true;
        s += length;
    }
    return false;
}

const ImGuiTextShaper::Cluster* ImGuiTextShaper::Next(const char* begin, const char* end)
{
    if (begin == end || (unsigned char)*begin < 0x80)
        return nullptr;
    // Measurement and wrapping usually consume a word in order. Reuse its shape
    // until the caller moves to another word or changes the visible text range.
    while (CurrentCluster < Clusters.Size && Clusters[CurrentCluster].Begin < begin)
        ++CurrentCluster;
    if (CurrentCluster < Clusters.Size && Clusters[CurrentCluster].Begin == begin && Clusters.back().End <= end)
        return &Clusters[CurrentCluster];

    unsigned int c;
    if (ImTextCharFromUtf8(&c, begin, end) == 0 || !IsDevanagari(c))
        return nullptr;
    const char* run_end = begin;
    while (run_end < end)
    {
        const int length = ImTextCharFromUtf8(&c, run_end, end);
        if (length == 0 || (!IsDevanagari(c) && c != 0x200c && c != 0x200d))
            break;
        run_end += length;
    }
    Clusters.clear();
    CurrentCluster = 0;
    if (!ImGuiFreeType::ShapeRun(Baked, begin, run_end, &Source, &Glyphs) || Glyphs.empty())
        return nullptr;

    for (int i = 0; i < Glyphs.Size; ++i)
    {
        const ImGuiShapedGlyph& glyph = Glyphs[i];
        const char* cluster_begin = begin + glyph.Cluster;
        // Unsafe boundaries may cross Unicode graphemes, for example an Indic
        // conjunct. Wrapping and ellipsis must keep the entire shaping unit.
        if (Clusters.empty() || (cluster_begin > Clusters.back().Begin && !glyph.UnsafeToBreak))
        {
            if (!Clusters.empty())
                Clusters.back().End = cluster_begin;
            Clusters.push_back({cluster_begin, run_end, i, 0, 0.0f});
        }
        ++Clusters.back().GlyphCount;
        Clusters.back().Advance += glyph.Advance;
    }
    return &Clusters.front();
}

void ImGuiTextShaper::Draw(ImDrawList* draw_list, const Cluster& cluster, const ImVec2& pos, float scale, ImU32 col, const ImVec4& clip_rect)
{
    float x = pos.x;
    for (int i = cluster.FirstGlyph; i < cluster.FirstGlyph + cluster.GlyphCount; ++i)
    {
        const ImGuiShapedGlyph& shaped = Glyphs[i];
        const ImFontGlyph* glyph = ImGuiFreeType::LoadGlyphIndex(Baked, Source, shaped.GlyphId);
        // Loading a glyph can grow the atlas and switch texture pages. Bind the
        // page that owns the just-loaded UVs before emitting its vertices.
        draw_list->PushTexture(Baked->OwnerFont->OwnerAtlas->TexRef);
        DrawGlyph(draw_list, glyph, ImVec2(x + shaped.Offset.x * scale, pos.y + shaped.Offset.y * scale), scale, col, clip_rect);
        draw_list->PopTexture();
        x += shaped.Advance * scale;
    }
}

const char* ImGuiTextShaper::ClusterBoundary(ImFont* font, float size, const char* begin, const char* end, const char* position, bool round_up)
{
    if (position <= begin)
        return begin;
    if (position >= end)
        return end;
    ImGuiTextShaper shaper(font->GetFontBaked(size));
    for (const char* s = begin; s < end;)
    {
        const Cluster* cluster = shaper.Next(s, end);
        const char* next = cluster ? cluster->End : s + ImTextCountUtf8BytesFromChar(s, end);
        if (next <= s)
            break;
        if (position > s && position < next)
            return round_up ? next : s;
        if (s >= position)
            return s;
        s = next;
    }
    return end;
}

void ImGuiTextShaper::RenderText(ImFont* font, ImDrawList* draw_list, float size, const ImVec2& pos, ImU32 col, const ImVec4& clip_rect, const char* begin, const char* end, float wrap_width, int flags)
{
    ImFontBaked* baked = font->GetFontBaked(size);
    ImGuiTextShaper shaper(baked);
    const float scale = size / baked->Size;
    const float origin_x = IM_TRUNC(pos.x);
    float x = origin_x;
    float y = IM_TRUNC(pos.y);
    const char* wrap_end = nullptr;
    for (const char* s = begin; s < end && y <= clip_rect.w;)
    {
        if (wrap_width > 0.0f)
        {
            if (!wrap_end)
                wrap_end = ImFontCalcWordWrapPositionEx(font, size, s, end, wrap_width - (x - origin_x), flags);
            if (s >= wrap_end)
            {
                x = origin_x;
                y += size;
                s = ImTextCalcWordWrapNextLineStart(s, end, flags);
                wrap_end = nullptr;
                continue;
            }
        }
        if (const Cluster* cluster = shaper.Next(s, end))
        {
            if (y + size >= clip_rect.y)
                shaper.Draw(draw_list, *cluster, ImVec2(x, y), scale, col, clip_rect);
            x += cluster->Advance * scale;
            s = cluster->End;
            continue;
        }
        unsigned int c;
        const int length = ImTextCharFromUtf8(&c, s, end);
        if (length == 0)
            break;
        s += length;
        if (c == '\n') { x = origin_x; y += size; wrap_end = nullptr; continue; }
        if (c == '\r')
            continue;
        const ImFontGlyph* glyph = baked->FindGlyph(c);
        const float advance = glyph->AdvanceX * scale;
        draw_list->PushTexture(font->OwnerAtlas->TexRef);
        DrawGlyph(draw_list, glyph, ImVec2(x, y), scale, col, clip_rect);
        draw_list->PopTexture();
        x += advance;
    }
}
#endif
