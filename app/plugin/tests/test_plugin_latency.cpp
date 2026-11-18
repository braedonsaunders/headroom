#include "core/HeadroomPluginCore.hpp"
#include "audio/RealtimeAudioServer.hpp"
#include "core/IComponent.hpp"
#include <cstdio>
#include <fstream>
#include <unistd.h>

int main() {
    char path[] = "/tmp/headroom-latency-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 1;
    close(fd);
    std::ofstream(path) << R"({"version":"0.5.4","architecture":"Linear","config":{"receptive_field":1,"bias":false},"weights":[1.0],"sample_rate":96000})";
    auto unit = rig::createComponentByTableId(900005);
    unit->prepare(48000, 512);
    unit->setStringParameter("capture_path", path);
    unit->processControlRate();
    auto expected = unit->latencySamples();
    headroom::plug::PluginCore core(headroom::plug::Format::Test, "");
    bool ok = core.prepare(48000, 512);
    rig::preset::RackPreset p;
    p.root_rack.table_id = 100;
    rig::preset::PresetComponentNode n;
    n.table_id = 900005;
    rig::preset::PresetParam param;
    param.id = "capture_path"; param.type = "string"; param.str_val = path;
    n.params.push_back(param);
    p.root_rack.children.push_back(n);
    rig::audio::RealtimeAudioServer::HostedState state;
    state.rig_xml = rig::library::userPresetXml(p);
    ok = ok && core.serverForTests()->restoreHostedState(state);
    const auto measured = core.latencySamples();
    ok = ok && expected > 0 && measured == expected;
    p.root_rack.children[0].table_id = 30000;
    p.root_rack.children[0].params.clear();
    state.rig_xml = rig::library::userPresetXml(p);
    ok = ok && core.serverForTests()->restoreHostedState(state) && core.latencySamples() == 0;
    std::printf("NAM latency: component %u, core %u; circuit amp: %u\n", expected, measured, core.latencySamples());
    unlink(path);
    return ok ? 0 : 1;
}
