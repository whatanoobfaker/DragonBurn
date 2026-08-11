#include "window_drag.h"

#include "imgui.h"

namespace window_drag
{
    void handle(HWND__* /*hwnd*/, const ImRect& drag_rect, bool block_drag)
    {
        static bool dragging = false;

        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        {
            dragging = false;
            return;
        }

        if (dragging)
        {
            const ImVec2 delta = ImGui::GetIO().MouseDelta;
            if (delta.x != 0.f || delta.y != 0.f)
                ImGui::SetWindowPos(ImGui::GetWindowPos() + delta);
            return;
        }

        if (block_drag || ImGui::IsAnyItemActive() || ImGui::IsAnyItemHovered())
            return;

        const ImVec2 mouse = ImGui::GetIO().MousePos;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && drag_rect.Contains(mouse))
            dragging = true;
    }
}
