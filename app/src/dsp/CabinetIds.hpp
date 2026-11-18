#pragma once
// Headroom's own ids for the built-in cabinets. Rigs store them as "matched:<id>" / "studio:<id>", and IR paths are
// built from them ("matched/<id>_A.ir", "studio/<id>/<id>_<mic>_<pos>.ir"). The table order is the stored parameter
// order of the legacy cabinet units (156000 "Cab", 119000 / 100000 "Cab" - 2), so it must never be reordered.
#include <array>
#include <cstdint>
#include <string_view>

namespace rig::dsp::cab_ids {

// Character class the IR synthesizer uses when no recorded impulse is installed.
enum class SynthVoice : uint8_t { Generic, Bass, TwoByTwelve, OneByTwelve };
// Speaker motional-impedance profile class (values in CabinetCatalog.hpp, speakerProfileForCab).
enum class Profile : uint8_t { None, Bass8x10, Bass15, BassAluminum, FourByTen, OneByTwelve, AlnicoTwoByTwelve, TwoByTwelve, Greenback, Modern, BritishV30 };

struct CabDef {
    const char* id;
    SynthVoice voice;
    Profile profile;
    uint32_t seed; // noise-seed state of this cabinet's synthesized fallback IRs (keeps each cabinet's voice stable)
};

// The 26 amp-matched cabinets (156000 "Cab" index).
inline constexpr std::array<CabDef, 26> kMatched = {{
    {"vox-ac30-2x12", SynthVoice::TwoByTwelve, Profile::AlnicoTwoByTwelve, 0x10de24aeu},
    {"ampeg-svt-810e", SynthVoice::Bass, Profile::Bass8x10, 0x28672465u},
    {"orange-ppc412", SynthVoice::Generic, Profile::BritishV30, 0xcd275413u},
    {"marshall-1960a-variac", SynthVoice::Generic, Profile::Greenback, 0x11bc3099u},
    {"mesa-recto-4x12", SynthVoice::Generic, Profile::Modern, 0xc0b458a6u},
    {"hiwatt-se4123", SynthVoice::Generic, Profile::BritishV30, 0x9f99ae4au},
    {"marshall-1960b", SynthVoice::Generic, Profile::Greenback, 0xe1204981u},
    {"soldano-4x12", SynthVoice::Generic, Profile::Modern, 0x32008f0fu},
    {"roland-jc120", SynthVoice::Generic, Profile::TwoByTwelve, 0xd6a94ed2u},
    {"marshall-1960a-g12t75", SynthVoice::Generic, Profile::None, 0x7fa368c1u},
    {"marshall-1960a-jcm800", SynthVoice::Generic, Profile::None, 0x351a6a61u},
    {"marshall-1960tv", SynthVoice::Generic, Profile::Greenback, 0xf319ab38u},
    {"mesa-oversized-4x12", SynthVoice::Generic, Profile::Modern, 0xe79b03d2u},
    {"fender-twin-2x12", SynthVoice::TwoByTwelve, Profile::TwoByTwelve, 0xaf3e9e64u},
    {"fender-tweed-deluxe", SynthVoice::OneByTwelve, Profile::OneByTwelve, 0x1740b6ebu},
    {"fender-bassman-4x10", SynthVoice::OneByTwelve, Profile::FourByTen, 0x9ca6dd32u},
    {"bogner-uberkab", SynthVoice::Generic, Profile::Modern, 0x714da78bu},
    {"peavey-5150-4x12", SynthVoice::Generic, Profile::Modern, 0x556224c0u},
    {"supro-1624t", SynthVoice::OneByTwelve, Profile::OneByTwelve, 0xfeed09fbu},
    {"ampeg-b15n", SynthVoice::Bass, Profile::Bass15, 0x364439dfu},
    {"friedman-be-4x12", SynthVoice::Generic, Profile::Modern, 0x8a7cd3e4u},
    {"bogner-ecstasy-4x12", SynthVoice::Generic, Profile::Modern, 0x768512b2u},
    {"soldano-slo-4x12", SynthVoice::Generic, Profile::Modern, 0x0e179f95u},
    {"vox-ac15-2x12", SynthVoice::TwoByTwelve, Profile::AlnicoTwoByTwelve, 0x68a61c7cu},
    {"fender-deluxe-reverb", SynthVoice::Generic, Profile::TwoByTwelve, 0xcc0b1592u},
    {"ampeg-svt-410hlf", SynthVoice::Bass, Profile::BassAluminum, 0x68b82bb7u},
}};

// The 29 studio cabinets (119000 / 100000 cabinet index - 2).
inline constexpr std::array<CabDef, 29> kStudio = {{
    {"vox-2x12-silver", SynthVoice::TwoByTwelve, Profile::AlnicoTwoByTwelve, 0xa2c10dd8u},
    {"vox-2x12-blue", SynthVoice::TwoByTwelve, Profile::AlnicoTwoByTwelve, 0x8186ec22u},
    {"orange-4x12-v30", SynthVoice::Generic, Profile::BritishV30, 0xa4958ac2u},
    {"mesa-recto-4x12-v30", SynthVoice::Generic, Profile::Modern, 0x90284146u},
    {"hiwatt-4x12-fane", SynthVoice::Generic, Profile::BritishV30, 0x039e93feu},
    {"marshall-68-4x12-greenback", SynthVoice::Generic, Profile::Greenback, 0x8f9a9372u},
    {"marshall-71-4x12-blackback", SynthVoice::Generic, Profile::None, 0xa4330ca0u},
    {"marshall-76-4x12-g12h30", SynthVoice::Generic, Profile::None, 0x2f89b948u},
    {"marshall-82-4x12-g12t75", SynthVoice::Generic, Profile::None, 0xf8d3a222u},
    {"marshall-84-4x12-g12-65", SynthVoice::Generic, Profile::None, 0x0ea28c56u},
    {"soldano-4x12-straight", SynthVoice::Generic, Profile::Modern, 0x4541b682u},
    {"soldano-4x12-slant", SynthVoice::Generic, Profile::Modern, 0xdc8fc476u},
    {"roland-jc120-2x12", SynthVoice::TwoByTwelve, Profile::TwoByTwelve, 0xbc80f6c8u},
    {"fender-twin-2x12-c12k", SynthVoice::TwoByTwelve, Profile::TwoByTwelve, 0x84aa3eb2u},
    {"fender-showman-2x15", SynthVoice::TwoByTwelve, Profile::Bass15, 0xb44694fau},
    {"fender-bassman-4x10-p10r", SynthVoice::OneByTwelve, Profile::FourByTen, 0x5bbbd302u},
    {"fender-tweed-1x12-p12q", SynthVoice::OneByTwelve, Profile::OneByTwelve, 0x1bab4312u},
    {"bogner-uberkab-4x12-v30", SynthVoice::Generic, Profile::Modern, 0xe8e3d01cu},
    {"bogner-uberkab-4x12-t75", SynthVoice::Generic, Profile::Modern, 0xa3a03d42u},
    {"peavey-5150-4x12-sheffield", SynthVoice::Generic, Profile::Modern, 0x62f30e56u},
    {"ampeg-svt-8x10", SynthVoice::Bass, Profile::Bass8x10, 0xe61a0392u},
    {"hartke-4x10-aluminum", SynthVoice::Bass, Profile::BassAluminum, 0xbf95401au},
    {"ampeg-b15-1x15", SynthVoice::Bass, Profile::Bass15, 0x7c0b4642u},
    {"mesa-lonestar-2x12", SynthVoice::TwoByTwelve, Profile::TwoByTwelve, 0xe77d661au},
    {"marshall-1936-2x12", SynthVoice::TwoByTwelve, Profile::AlnicoTwoByTwelve, 0x9cc590dau},
    {"marshall-1960tv-4x12", SynthVoice::Generic, Profile::None, 0x0008dceau},
    {"bogner-412st-4x12", SynthVoice::Generic, Profile::Modern, 0xe4362fd4u},
    {"mesa-oversized-4x12-a", SynthVoice::Generic, Profile::Modern, 0x6fd9f69au},
    {"mesa-oversized-4x12-b", SynthVoice::Generic, Profile::Modern, 0x03a210e2u},
}};

// Noise-seed state of the plate & spring fallback IRs.
inline constexpr uint32_t kPlateSpringSeed = 0x787c3e33u;

template <std::size_t N>
constexpr int indexOf(const std::array<CabDef, N>& t, std::string_view id) noexcept {
    for (std::size_t i = 0; i < N; ++i)
        if (id == t[i].id) return static_cast<int>(i);
    return -1;
}
constexpr int matchedIndex(std::string_view id) noexcept { return indexOf(kMatched, id); }
constexpr int studioIndex(std::string_view id) noexcept { return indexOf(kStudio, id); }

// The table entry for "matched:<id>", "studio:<id>" or a bare id (matched ids first), or nullptr.
constexpr const CabDef* find(std::string_view id) noexcept {
    if (id.rfind("matched:", 0) == 0) { const int i = matchedIndex(id.substr(8)); return i < 0 ? nullptr : &kMatched[static_cast<std::size_t>(i)]; }
    if (id.rfind("studio:", 0) == 0) { const int i = studioIndex(id.substr(7)); return i < 0 ? nullptr : &kStudio[static_cast<std::size_t>(i)]; }
    if (const int i = matchedIndex(id); i >= 0) return &kMatched[static_cast<std::size_t>(i)];
    if (const int i = studioIndex(id); i >= 0) return &kStudio[static_cast<std::size_t>(i)];
    return nullptr;
}

} // namespace rig::dsp::cab_ids
