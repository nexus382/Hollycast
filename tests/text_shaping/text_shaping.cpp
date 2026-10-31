// Portions Copyright 2026 The Hollycast Authors
#include "imgui.h"
#include "imgui_internal.h"
#include "misc/freetype/imgui_freetype.h"
#include "misc/freetype/imgui_text_shaping.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>
#include <cmath>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

static void Require(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// Independent FT/HarfBuzz objects verify the actual substitutions, offsets and
// advances, rather than only checking that Unicode characters have glyphs.
class ReferenceShape
{
public:
    FT_Library Library = nullptr;
    FT_Face Face = nullptr;
    hb_font_t* Font = nullptr;
    hb_buffer_t* Buffer = nullptr;
    explicit ReferenceShape(const std::string& filename)
    {
        Require(FT_Init_FreeType(&Library) == 0, "reference FreeType init failed");
        Require(FT_New_Face(Library, filename.c_str(), 0, &Face) == 0, "reference font failed");
        Font = hb_ft_font_create_referenced(Face);
        hb_ft_font_set_load_flags(Font, FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_NORMAL);
        Buffer = hb_buffer_create();
    }
    ~ReferenceShape()
    {
        hb_buffer_destroy(Buffer);
        hb_font_destroy(Font);
        FT_Done_Face(Face);
        FT_Done_FreeType(Library);
    }
    float Shape(const std::string& text, float size)
    {
        FT_Size_RequestRec request{};
        request.type = FT_SIZE_REQUEST_TYPE_REAL_DIM;
        request.height = (FT_Long)(size * 64);
        Require(FT_Request_Size(Face, &request) == 0, "reference size failed");
        hb_ft_font_changed(Font);
        hb_buffer_reset(Buffer);
        hb_buffer_set_direction(Buffer, HB_DIRECTION_LTR);
        hb_buffer_set_script(Buffer, HB_SCRIPT_DEVANAGARI);
        hb_buffer_set_language(Buffer, hb_language_from_string("hi", -1));
        hb_buffer_add_utf8(Buffer, text.data(), (int)text.size(), 0, (int)text.size());
        hb_shape(Font, Buffer, nullptr, 0);
        unsigned count;
        const auto* positions = hb_buffer_get_glyph_positions(Buffer, &count);
        float width = 0;
        for (unsigned i = 0; i < count; ++i)
            width += positions[i].x_advance / 64.0f;
        return width;
    }
};

int main(int argc, char** argv)
{
    try
    {
        Require(argc == 2, "expected font directory");
        const std::string directory = std::string(argv[1]) + "/";
        ReferenceShape reference(directory + "NotoSansDevanagari-Medium.ttf");
        const std::vector<std::string> words = {
            "हिन्दी", "हिंदी", "क्षि", "क्षेत्र", "प्रदर्शन", "फ़ाइल", "सेटिंग्स", "क्रिया",
            "श्रद्धा", "विज्ञापन", "किं", "क़ी", "र्कि", "क्‍ष", "क्‌ष"
        };
        size_t checks = 0;
        for (int reload = 0; reload < 3; ++reload)
        {
            ImGui::CreateContext();
            auto& io = ImGui::GetIO();
            io.IniFilename = nullptr;
            io.DisplaySize = ImVec2(1280, 800);
            io.DeltaTime = 1.0f / 60.0f;
            io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
            io.Fonts->SetFontLoader(ImGuiFreeType::GetFontLoader());
            std::vector<ImFont*> fonts;
            auto add = [&](const char* filename, float size, int face) {
                ImFontConfig primary; primary.FontNo = face;
                auto* font = io.Fonts->AddFontFromFileTTF((directory + filename).c_str(), size, &primary);
                Require(font != nullptr, "primary font failed");
                ImFontConfig fallback; fallback.MergeMode = true; fallback.DstFont = font;
                Require(io.Fonts->AddFontFromFileTTF((directory + "NotoSansDevanagari-Medium.ttf").c_str(), size, &fallback) != nullptr, "Hindi fallback failed");
                fonts.push_back(font);
            };
            add("Roboto-Medium.ttf", 17, 0);
            add("Roboto-Bold.ttf", 17, 0);
            add("Roboto-Regular.ttf", 21, 0);
            add("EncodeSans-wdth-wght.ttf", 21, 5 << 16);
            add("EncodeSans-wdth-wght.ttf", 24, 5 << 16);
            io.Fonts->Build();
            ImGui::NewFrame();
            for (auto* font : fonts)
            {
                for (float size : {13.0f, 17.0f, 21.0f, 24.0f, 32.0f})
                {
                    auto* baked = font->GetFontBaked(size);
                    for (const auto& word : words)
                    {
                        const float expected_width = reference.Shape(word, size);
                        ImVector<ImGuiShapedGlyph> glyphs;
                        ImFontConfig* source = nullptr;
                        Require(ImGuiFreeType::ShapeRun(baked, word.data(), word.data() + word.size(), &source, &glyphs), "run did not shape");
                        unsigned count;
                        const auto* infos = hb_buffer_get_glyph_infos(reference.Buffer, &count);
                        const auto* positions = hb_buffer_get_glyph_positions(reference.Buffer, nullptr);
                        Require(glyphs.Size == (int)count, "shaped glyph count differs");
                        std::set<const char*> boundaries{word.data(), word.data() + word.size()};
                        for (unsigned i = 0; i < count; ++i)
                        {
                            Require(glyphs[i].GlyphId != 0 && glyphs[i].GlyphId == infos[i].codepoint, "missing or wrong substituted glyph");
                            Require(std::abs(glyphs[i].Advance - positions[i].x_advance / 64.0f) < 0.001f, "wrong advance");
                            Require(std::abs(glyphs[i].Offset.x - positions[i].x_offset / 64.0f) < 0.001f, "wrong horizontal mark offset");
                            Require(std::abs(glyphs[i].Offset.y + positions[i].y_offset / 64.0f) < 0.001f, "wrong vertical mark offset");
                            if (!(hb_glyph_info_get_glyph_flags(&infos[i]) & HB_GLYPH_FLAG_UNSAFE_TO_BREAK))
                                boundaries.insert(word.data() + infos[i].cluster);
                            Require(ImGuiFreeType::LoadGlyphIndex(baked, source, glyphs[i].GlyphId) != nullptr, "substituted glyph failed to rasterize");
                        }
                        Require(std::abs(font->CalcTextSizeA(size, FLT_MAX, 0, word.c_str()).x - expected_width) < 0.001f, "measurement ignores shaping");
                        const std::string mixed = "Hollycast " + word + " 60 Hz";
                        const float mixed_width = font->CalcTextSizeA(size, FLT_MAX, 0, "Hollycast ").x + expected_width + font->CalcTextSizeA(size, FLT_MAX, 0, " 60 Hz").x;
                        Require(std::abs(font->CalcTextSizeA(size, FLT_MAX, 0, mixed.c_str()).x - mixed_width) < 0.001f, "mixed-script measurement differs");
                        for (int width = 1; width <= 300; ++width)
                        {
                            const char* end = font->CalcWordWrapPosition(size, word.data(), word.data() + word.size(), (float)width);
                            Require(end > word.data() && boundaries.count(end), "wrap split an Indic shaping cluster");
                            const char* remaining = nullptr;
                            font->CalcTextSizeA(size, (float)width, 0, word.data(), word.data() + word.size(), &remaining);
                            Require(boundaries.count(remaining), "clipping split an Indic shaping cluster");
                            ++checks;
                        }
                        ImDrawList draw_list(ImGui::GetDrawListSharedData());
                        draw_list._ResetForNewFrame();
                        draw_list.PushClipRect(ImVec2(0, 0), ImVec2(1200, 600));
                        draw_list.PushTexture(io.Fonts->TexRef);
                        font->RenderText(&draw_list, size, ImVec2(10, 10), IM_COL32_WHITE, ImVec4(0, 0, 1200, 600), mixed.data(), mixed.data() + mixed.size(), 75);
                        Require(draw_list.VtxBuffer.Size > 0 && draw_list.IdxBuffer.Size > 0, "shaped text produced no geometry");
                        for (const auto& vertex : draw_list.VtxBuffer)
                            Require(std::isfinite(vertex.pos.x) && std::isfinite(vertex.pos.y) && vertex.uv.x >= 0 && vertex.uv.x <= 1 && vertex.uv.y >= 0 && vertex.uv.y <= 1, "invalid glyph vertices");
                        const int cached_size = baked->Glyphs.Size;
                        for (const auto& glyph : glyphs)
                            Require(ImGuiFreeType::LoadGlyphIndex(baked, source, glyph.GlyphId) != nullptr, "cached glyph disappeared");
                        Require(baked->Glyphs.Size == cached_size, "glyph cache grows on repeated rendering");
                    }
                }
            }
            ImGui::EndFrame();
            ImGui::DestroyContext();
        }
        std::cout << checks << " cluster wrap/clip cases passed; substitutions, offsets, mixed text, rendering, sizes and reloads passed\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
