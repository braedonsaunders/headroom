#pragma once
#include "../core/IComponent.hpp"
#include <memory>
#include <string>

namespace rig {

std::unique_ptr<IComponent> makeNamComponent(const catalog::ComponentSchema& schema);
std::string namCaptureFolder();   // ~/Library/Application Support/Headroom/NAM Captures
void rescanNamCaptures();         // pick up newly imported .nam files
// True when the capture is an amp head (needs a cabinet after it); false for full rigs / pedals.
bool namCaptureWantsCab(int index);

} // namespace rig
