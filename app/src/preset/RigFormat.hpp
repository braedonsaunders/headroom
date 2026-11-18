#pragma once
// Headroom's .rig preset file: its element names and file extension (read by PresetLoader.cpp, written by
// library/Library.cpp).
#include <string_view>

namespace rig::preset::format {

inline constexpr std::string_view kInfoOpen = "<headroom-info";   // preset name / author / comment block
inline constexpr std::string_view kInfoClose = "</headroom-info>";
inline constexpr std::string_view kRigOpen = "<headroom-rig";     // the rig document
inline constexpr std::string_view kRigClose = "</headroom-rig>";
inline constexpr std::string_view kRigTag = "headroom-rig";       // root element
inline constexpr std::string_view kRackTag = "rack";              // the rack element inside it
inline constexpr std::string_view kRigExtension = ".rig";

} // namespace rig::preset::format
