#include "features/catalog.h"

#include <cstdio>
#include <cstring>

#include <imgui.h>

#include "core/util.h"

namespace cg::features {

bool DrawCatalog(const char* id, std::span<const CatalogItem> items, char* selected, size_t selectedSize, char* filter, size_t filterSize,
                 float rows) {
  bool activated = false;
  ImGui::PushID(id);
  ImGui::SetNextItemWidth(-1.0f);
  ImGui::InputTextWithHint("##filter", "Filter the list (optional)", filter, filterSize);
  const float height = ImGui::GetTextLineHeightWithSpacing() * rows + ImGui::GetStyle().WindowPadding.y * 2.0f;
  if (ImGui::BeginChild("##list", ImVec2(0.0f, height), ImGuiChildFlags_Borders)) {
    const char* group = nullptr;
    size_t shown = 0;
    for (const CatalogItem& it : items) {
      if (filter[0] && !util::IContains(it.name, filter) && !util::IContains(it.id, filter) && !util::IContains(it.group, filter)) continue;
      if (group == nullptr || std::strcmp(group, it.group) != 0) {
        group = it.group;
        ImGui::SeparatorText(group);
      }
      ++shown;
      const bool isSelected = std::strcmp(selected, it.id) == 0;
      if (ImGui::Selectable(it.name, isSelected, ImGuiSelectableFlags_AllowDoubleClick)) {
        std::snprintf(selected, selectedSize, "%s", it.id);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) activated = true;
      }
      if (isSelected && ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Enter)) activated = true;
      ImGui::SetItemTooltip("%s", it.id);
    }
    if (shown == 0) ImGui::TextDisabled("Nothing matches \"%s\".", filter);
  }
  ImGui::EndChild();
  ImGui::PopID();
  return activated;
}

std::string CatalogName(std::span<const CatalogItem> items, const std::string& id) {
  for (const CatalogItem& it : items)
    if (id == it.id) return it.name;
  return {};
}

}  // namespace cg::features
