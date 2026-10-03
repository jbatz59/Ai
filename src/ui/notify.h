#pragma once
// Toast notifications (bottom-right stack, animated). Thread-safe Push().
#include <string>

namespace cg::ui::notify {

enum class Kind { Info, Success, Warning, Error };

void Push(Kind kind, std::string title, std::string text = {}, float seconds = 3.5f);
void Draw();   // overlay calls once per frame (visible even when the menu is closed)

}  // namespace cg::ui::notify
