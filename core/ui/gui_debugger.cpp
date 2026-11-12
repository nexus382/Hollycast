// Portions Copyright 2026 The Hollycast Authors
#include "gui_debugger.h"
#include "gui_util.h"
#include "debug/assembly_debugger.h"
#include "hw/sh4/sh4_opcode_list.h"
#include <array>
#include <cstdio>

using i18n::T;
using i18n::Ts;

namespace {

// A bounded address page keeps row positions well within ImGui's float precision.
// Changing PC never changes the page or scroll offset unless explicitly requested.
constexpr u32 pageBytes = 0x10000;
constexpr int pageRows = pageBytes / 2;
u32 pageBase;
u32 selectedAddress;
u32 scrollAddress;
bool scrollRequested;
bool refreshRequested;
bool addressError;
bool breakpointError;
char addressText[11]{};
std::array<u16, pageRows> opcodes{};
std::array<bool, pageRows> readable{};

void goTo(u32 address)
{
    pageBase = address & ~(pageBytes - 1);
    selectedAddress = scrollAddress = address;
    scrollRequested = refreshRequested = true;
    addressError = false;
    breakpointError = false;
    std::snprintf(addressText, sizeof(addressText), "%08X", address);
}

void refreshPage()
{
    for (int row = 0; row < pageRows; row++)
        readable[row] = assembly_debugger::readInstruction(pageBase + row * 2, opcodes[row]);
    refreshRequested = false;
}

}

void gui_open_debugger()
{
    if (!assembly_debugger::open())
        return;
    goTo(Sh4cntx.pc);
    gui_setState(GuiState::Debugger);
}

void gui_poll_debugger()
{
    if (!assembly_debugger::active())
        return;
    if (!assembly_debugger::available())
    {
        assembly_debugger::close();
        if (gui_state == GuiState::Debugger)
            gui_setState(GuiState::Commands);
        return;
    }
    if (assembly_debugger::breakPending())
    {
        // Join the execution thread before inspecting registers or RAM.
        emu.stop();
        assembly_debugger::acknowledgeBreak();
        goTo(Sh4cntx.pc);
        gui_setState(GuiState::Debugger);
    }
}

void gui_display_debugger()
{
    if (!assembly_debugger::active())
        return;
    const bool running = emu.running();
    if (running)
    {
        ImGui::SetNextWindowPos(ImVec2(uiScaled(20), uiScaled(55)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowBgAlpha(0.9f);
        const std::string title = Ts("Assembly Debugger") + "###AssemblyDebuggerRunning";
        ImGui::Begin(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextUnformatted(T("Running with debugger enabled"));
        if (ImGui::Button(T("Pause")))
            gui_open_debugger();
        ImGui::SameLine();
        if (ImGui::Button(T("Close Debugger")))
        {
            assembly_debugger::close();
            emu.start();
        }
        ImGui::End();
        return;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.04f, display.y * 0.06f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(display.x * 0.92f, display.y * 0.88f), ImGuiCond_FirstUseEver);
    bool open = true;
    const std::string title = Ts("Assembly Debugger") + "###AssemblyDebugger";
    if (!ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse))
    {
        ImGui::End();
        if (!open)
        {
            assembly_debugger::close();
            gui_setState(GuiState::Commands);
        }
        return;
    }
    if (ImGui::Button(T("Resume")))
    {
        assembly_debugger::resume();
        gui_setState(GuiState::Closed);
        ImGui::End();
        return;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Step")))
    {
        assembly_debugger::step();
        goTo(Sh4cntx.pc);
    }
    if (ImGui::Button(T("Current Instruction")))
        goTo(Sh4cntx.pc);
    ImGui::SameLine();
    if (ImGui::Button(T("Close Debugger")))
        open = false;

    ImGui::Text("%s  PC: %08X  PR: %08X  SR: %08X", T("Paused"), Sh4cntx.pc, Sh4cntx.pr, Sh4cntx.sr.getFull());
    ImGui::TextWrapped("%s", T("Taken delayed branches step with their delay slots. A breakpoint in that slot stops before the branch."));

    ImGui::SetNextItemWidth(uiScaled(135));
    const bool enter = ImGui::InputText("##assemblyAddress", addressText, sizeof(addressText), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button(T("Go to Address")) || enter)
    {
        u32 address;
        addressError = !assembly_debugger::parseAddress(addressText, address);
        if (!addressError)
            goTo(address);
    }
    if (addressError)
        ImGui::TextUnformatted(T("Enter an even hexadecimal address, for example 8C010000."));
    {
        DisabledScope disabled{pageBase < pageBytes};
        if (ImGui::Button("< 64 KiB"))
            goTo(pageBase - pageBytes);
    }
    ImGui::SameLine();
    {
        DisabledScope disabled{pageBase == 0xffff0000};
        if (ImGui::Button("64 KiB >"))
            goTo(pageBase + pageBytes);
    }
    ImGui::SameLine();
    ImGui::Text("%08X - %08X", pageBase, pageBase + pageBytes - 2);

    if (refreshRequested)
        refreshPage();
    const auto& points = assembly_debugger::breakpoints();
    if (ImGui::BeginTable("assemblyLayout", 2, ImGuiTableFlags_Resizable))
    {
        ImGui::TableSetupColumn("Assembly", ImGuiTableColumnFlags_WidthStretch, 3.f);
        ImGui::TableSetupColumn("Registers", ImGuiTableColumnFlags_WidthStretch, 1.f);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        const float paneHeight = std::max(uiScaled(100), ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing());
        ImGui::BeginChild("assemblyRows", ImVec2(0, paneHeight), ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        const float rowHeight = ImGui::GetTextLineHeightWithSpacing();
        if (scrollRequested)
        {
            ImGui::SetScrollY(std::max(0.f, (scrollAddress - pageBase) / 2 * rowHeight - paneHeight * 0.3f));
            scrollRequested = false;
        }
        ImGuiListClipper clipper;
        clipper.Begin(pageRows, rowHeight);
        while (clipper.Step())
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++)
            {
                const u32 address = pageBase + row * 2;
                const bool current = address == Sh4cntx.pc;
                const bool breakpoint = points.count(address) != 0;
                char text[512];
                char instruction[256];
                if (readable[row])
                {
                    OpDesc[opcodes[row]]->Disassemble(instruction, address, opcodes[row]);
                    std::snprintf(text, sizeof(text), "%s %s %08X  %04X  %s", current ? ">" : " ", breakpoint ? "B" : " ", address, opcodes[row], instruction);
                }
                else
                    std::snprintf(text, sizeof(text), "%s   %08X  ----  %s", current ? ">" : " ", address, T("Unavailable"));
                ImGui::PushID(row);
                if (current)
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.85f, 0.25f, 1.f));
                if (ImGui::Selectable(text, current || selectedAddress == address, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, ImGui::GetTextLineHeight())))
                {
                    selectedAddress = address;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && readable[row])
                        assembly_debugger::setBreakpoint(address, !breakpoint);
                }
                if (current)
                    ImGui::PopStyleColor();
                if (ImGui::BeginPopupContextItem())
                {
                    if (ImGui::MenuItem(T("Breakpoint"), nullptr, breakpoint, readable[row]))
                        assembly_debugger::setBreakpoint(address, !breakpoint);
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        ImGui::EndChild();
        ImGui::TableNextColumn();
        ImGui::BeginChild("assemblyRegisters", ImVec2(0, paneHeight));
        for (int reg = 0; reg < 16; reg++)
            ImGui::Text("R%-2d %08X", reg, Sh4cntx.r[reg]);
        ImGui::Separator();
        ImGui::TextUnformatted(T("Breakpoints"));
        // Removal occurs after iteration so no iterator can be invalidated.
        u32 removeAddress = 0;
        bool remove = false;
        for (u32 address : points)
        {
            ImGui::PushID(static_cast<int>(address));
            char label[12];
            std::snprintf(label, sizeof(label), "%08X", address);
            // Keep the address hit area clear of the adjacent remove button.
            if (ImGui::Selectable(label, false, ImGuiSelectableFlags_None, ImVec2(ImGui::CalcTextSize(label).x, 0)))
                goTo(address);
            ImGui::SameLine();
            if (ImGui::SmallButton("X"))
            {
                removeAddress = address;
                remove = true;
            }
            ImGui::PopID();
        }
        if (remove)
            assembly_debugger::setBreakpoint(removeAddress, false);
        ImGui::EndChild();
        ImGui::EndTable();
    }
    const bool hasBreakpoint = points.count(selectedAddress) != 0;
    if (ImGui::Button(hasBreakpoint ? T("Remove Breakpoint") : T("Add Breakpoint")))
        breakpointError = !assembly_debugger::setBreakpoint(selectedAddress, !hasBreakpoint);
    ImGui::SameLine();
    ImGui::Text("%08X", selectedAddress);
    if (breakpointError)
        ImGui::TextWrapped("%s", T("Breakpoints require a readable instruction address."));
    ImGui::End();
    if (!open)
    {
        assembly_debugger::close();
        gui_setState(GuiState::Commands);
    }
}
