#pragma once
// Loads system fonts at runtime (nothing redistributed): Segoe UI (+ Semibold/Bold), Consolas for
// monospace, and merges Segoe Fluent Icons / Segoe MDL2 Assets glyphs into the body font when
// present. Falls back to ImGui's embedded font on any failure.
#include "render/overlay.h"

namespace cg::render::fonts {

// Reference sizes in unscaled pixels. Dear ImGui 1.92 rasterises glyphs on demand at whatever size
// is in use, so the global UI scale is applied through style.FontScaleMain (theme::Apply), never by
// loading bigger fonts. To draw at a multiple of the body size use
// ImGui::PushFont(font, ImGui::GetStyle().FontSizeBase * factor).
inline constexpr float kBodySize = 16.0f;
inline constexpr float kTitleSize = 26.0f;
inline constexpr float kMonoSize = 15.0f;

// Must run after ImGui::CreateContext and before the first NewFrame. `scale` = UI scale; it is only
// logged — sizing is dynamic (see above). Sets io.FontDefault to the body font.
void Load(float scale, Fonts& out);

}  // namespace cg::render::fonts

// Icon glyphs, UTF-8 encoded. Every codepoint below is listed with the same name in BOTH Microsoft's
// "Segoe Fluent Icons font" (Windows 11, SegoeIcons.ttf) and "Segoe MDL2 Assets" (Windows 10,
// segmdl2.ttf) reference tables, so whichever font is merged renders the same symbol.
// Only valid to render when GetFonts().hasIcons; ui::Icon() handles the fallback.
#define CG_ICON_PLAYER        "\xee\x9d\xbb"   // U+E77B Contact
#define CG_ICON_CAR           "\xee\xa0\x84"   // U+E804 Car
#define CG_ICON_WEAPON        "\xef\x89\xb2"   // U+F272 Bullseye
#define CG_ICON_WORLD         "\xee\xa4\x89"   // U+E909 World
#define CG_ICON_TELEPORT      "\xee\x9c\x87"   // U+E707 MapPin
#define CG_ICON_CAMERA        "\xee\x9c\xa2"   // U+E722 Camera
#define CG_ICON_VISUALS       "\xee\xa2\x90"   // U+E890 View (eye)
#define CG_ICON_TABLE         "\xee\xa7\x95"   // U+E9D5 CheckList
#define CG_ICON_SCRIPT        "\xee\xa5\x83"   // U+E943 Code
#define CG_ICON_TOOLS         "\xee\xa4\x8f"   // U+E90F Repair (wrench)
#define CG_ICON_SETTINGS      "\xee\x9c\x93"   // U+E713 Settings (MDL2 name: Setting)
#define CG_ICON_CONSOLE       "\xee\x9d\x96"   // U+E756 CommandPrompt
#define CG_ICON_INFO          "\xee\xa5\x86"   // U+E946 Info
#define CG_ICON_SEARCH        "\xee\x9c\xa1"   // U+E721 Search
#define CG_ICON_STAR          "\xee\x9c\xb4"   // U+E734 FavoriteStar (outline)
#define CG_ICON_WARNING       "\xee\x9e\xba"   // U+E7BA Warning
#define CG_ICON_CHECK         "\xee\x9c\xbe"   // U+E73E CheckMark
#define CG_ICON_CLOSE         "\xee\xa2\xbb"   // U+E8BB ChromeClose
#define CG_ICON_KEYBOARD      "\xee\x9d\xa5"   // U+E765 KeyboardClassic
#define CG_ICON_HOME          "\xee\xa0\x8f"   // U+E80F Home
#define CG_ICON_STAR_FILL     "\xee\x9c\xb5"   // U+E735 FavoriteStarFill
#define CG_ICON_PIN           "\xee\x9c\x98"   // U+E718 Pin
#define CG_ICON_PINNED        "\xee\xa1\x80"   // U+E840 Pinned
#define CG_ICON_POWER         "\xee\x9f\xa8"   // U+E7E8 PowerButton
#define CG_ICON_REFRESH       "\xee\x9c\xac"   // U+E72C Refresh
#define CG_ICON_PLAY          "\xee\x9d\xa8"   // U+E768 Play
#define CG_ICON_DELETE        "\xee\x9d\x8d"   // U+E74D Delete
#define CG_ICON_COPY          "\xee\xa3\x88"   // U+E8C8 Copy
#define CG_ICON_FOLDER        "\xee\xa0\xb8"   // U+E838 FolderOpen
#define CG_ICON_ERROR         "\xee\x9e\x83"   // U+E783 Error
#define CG_ICON_LIGHTNING     "\xee\xa5\x85"   // U+E945 LightningBolt
#define CG_ICON_BUG           "\xee\xaf\xa8"   // U+EBE8 Bug
#define CG_ICON_DEVTOOLS      "\xee\xb1\xba"   // U+EC7A DeveloperTools
#define CG_ICON_HEALTH        "\xee\xa5\x9e"   // U+E95E Health
#define CG_ICON_CHEVRON_RIGHT "\xee\x9d\xac"   // U+E76C ChevronRight
#define CG_ICON_CHEVRON_DOWN  "\xee\x9c\x8d"   // U+E70D ChevronDown
#define CG_ICON_SAVE          "\xee\x9d\x8e"   // U+E74E Save
#define CG_ICON_ADD           "\xee\x9c\x90"   // U+E710 Add
#define CG_ICON_LIST          "\xee\xa8\xb7"   // U+EA37 List
#define CG_ICON_GRID          "\xef\x83\xa2"   // U+F0E2 GridView
#define CG_ICON_LINK          "\xee\x9c\x9b"   // U+E71B Link
#define CG_ICON_HELP          "\xee\xa2\x97"   // U+E897 Help
#define CG_ICON_SHIELD        "\xee\xa8\x98"   // U+EA18 Shield
#define CG_ICON_GAME          "\xee\x9f\xbc"   // U+E7FC Game (controller)
#define CG_ICON_OPEN_WINDOW   "\xee\xa2\xa7"   // U+E8A7 OpenInNewWindow
#define CG_ICON_LOCK          "\xee\x9c\xae"   // U+E72E Lock
#define CG_ICON_HISTORY       "\xee\xa0\x9c"   // U+E81C History
#define CG_ICON_STOPWATCH     "\xee\xa4\x96"   // U+E916 Stopwatch
#define CG_ICON_LOCATION      "\xee\xa0\x9d"   // U+E81D Location
#define CG_ICON_SPEED         "\xee\xb1\x8a"   // U+EC4A SpeedHigh
#define CG_ICON_EDIT          "\xee\x9c\x8f"   // U+E70F Edit
#define CG_ICON_ACCEPT        "\xee\xa3\xbb"   // U+E8FB Accept
