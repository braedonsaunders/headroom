// preset_rt: renders one rig the way the standalone app does and times every callback against its deadline.
//
//   * The render path is the app's: RackGraph::buildFromPreset (parallel prepare), syncCabFollow, a published
//     renderSnapshot plan with the unit meter bank armed (the UI polls meters while it is open), series pipelining
//     and split lanes on RtPool.
//   * A real-time caller: one thread with a Mach time-constraint policy (period = the buffer) wakes on an absolute
//     grid every buffer period, the way the HAL IO thread does, and renders one buffer of the DI player's signal
//     (decoded, resampled to 48 kHz and peak-normalised to 0.35 FS exactly as RealtimeAudioServer::decodeTrack does,
//     mono on both channels).
//   * The caller and RtPool's workers are in one audio work interval (AudioWorkIntervalCreate), as AudioIO joins the
//     pool to the device's IO-thread workgroup; each buffer is an os_workgroup_interval start/finish.
//   * A deadline is missed when a callback finishes after its scheduled wake + one period (the grid then resyncs,
//     like the HAL after an overload). Warm-up buffers are excluded from every statistic.
//   * Per-unit time: RackGraph keeps each node's smoothed per-block render time (cost_us, the stage balancer's input,
//     an exponential average updated once per block). Read after every block, the average is inverted to the exact
//     per-block time of each unit (processAudio only, wherever it ran).
//   * --solo: afterwards each unit's processAudio is timed alone, off the graph, at call sizes 16..256 frames, and
//     the cost is fitted as a + b*frames: `a` is the fixed per-call cost (what sub-blocks and small buffers pay).
//
// Output: one JSON object on stdout. Measurement tool only; it never writes outside stdout.
//
//   preset_rt --root <repo root> --rig <file.rig> --di a.wav,b.wav,... [--frames 64,128,256] [--seconds 10]
//             [--warmup 2] [--seg-offset 2] [--nopipe] [--solo] [--solo-tids 16000,81000] [--dump blocks.csv]
//
// Driver over every rig: run_all.py (one rig at a time, HEADROOM_DATA_DIR always an isolated copy); tables: analyze.py.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <AudioToolbox/AudioWorkInterval.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <os/workgroup.h>
#include <pthread.h>
#include <stdlib.h>

#include "audio/WavIO.hpp"
#include "core/RackGraph.hpp"
#include "core/RtPool.hpp"
#include "core/UnitMeters.hpp"
#include "generated/ComponentCatalog.hpp"
#include "midi/MidiComponents.hpp"
#include "midi/MidiEvent.hpp"
#include "platform/Paths.hpp"
#include "preset/PresetLoader.hpp"

// ── read access to RackGraph's node tree (measurement only; the engine is not modified) ──
// The explicit instantiation below may name a private member; the friend it defines hands the pointer out.
template <auto M>
struct NodesRob {
    friend auto rackNodes() { return M; }
};
auto rackNodes();
template struct NodesRob<&rig::RackGraph::nodes_>;

namespace {

double g_ticks_per_us = 1.0;
uint64_t usToTicks(double us) { return static_cast<uint64_t>(us * g_ticks_per_us); }
double ticksToUs(uint64_t t) { return static_cast<double>(t) / g_ticks_per_us; }

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string it;
    while (std::getline(ss, it, c))
        if (!it.empty()) out.push_back(it);
    return out;
}

std::string jsonEsc(std::string_view s) {
    std::string o;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') o += '\\', o += ch;
        else if (static_cast<unsigned char>(ch) < 0x20) o += ' ';
        else o += ch;
    }
    return o;
}

// RealtimeAudioServer::decodeTrack: mono, linear resample to the engine rate, 0.35 FS peak.
bool decodeTrack(const std::string& path, double rate, std::vector<float>& out) {
    rig::audio::WavData wav;
    if (!rig::audio::readWavFile(path, wav) || wav.left.empty()) return false;
    const double ratio = double(wav.sample_rate) / rate;
    const std::size_t n = static_cast<std::size_t>(double(wav.left.size()) / ratio);
    std::vector<float> tmp(n);
    float peak = 0.0f;
    const std::size_t last = wav.left.size() - 1;
    for (std::size_t i = 0; i < n; ++i) {
        const double src = double(i) * ratio;
        const std::size_t i0 = std::min(static_cast<std::size_t>(src), last), i1 = std::min(i0 + 1, last);
        const float fr = static_cast<float>(src - double(i0));
        const float a = 0.5f * (wav.left[i0] + wav.right[i0]), b = 0.5f * (wav.left[i1] + wav.right[i1]);
        tmp[i] = a + fr * (b - a);
        peak = std::max(peak, std::abs(tmp[i]));
    }
    const float g = peak > 1e-4f ? 0.35f / peak : 1.0f;
    out.resize(n);
    for (std::size_t i = 0; i < n; ++i) // int16 round trip, as TrackPCM stores it
        out[i] = static_cast<float>(std::lround(std::clamp(tmp[i] * g, -1.0f, 1.0f) * 32767.0f)) / 32767.0f;
    return true;
}

struct NodeInfo {
    void* node = nullptr;       // GraphNode*
    double* cost = nullptr;     // &GraphNode::cost_us
    rig::IComponent* comp = nullptr;
    int32_t tid = 0;
    std::string name, lane;
    int order = 0;
    bool bypassed = false, utility = false, structural = false;
    int safety = 0;
    bool mono_pres = false;
    double prev = 0.0;
    std::vector<float> raw; // per measured block, µs
};

template <class List>
void walk(List& list, const std::string& lane, std::vector<NodeInfo>& out, int& split_ctr) {
    std::vector<std::string> stack{lane};
    std::vector<int> ids;
    for (auto& up : list) {
        auto& n = *up;
        const int32_t tid = n.table_id;
        std::string cur = stack.back();
        NodeInfo nfo;
        nfo.node = &n;
        nfo.cost = &n.cost_us;
        nfo.comp = n.component.get();
        nfo.tid = tid;
        const auto* sch = rig::catalog::findByTableId(tid);
        nfo.name = sch ? std::string(sch->reference_name) : ("tid " + std::to_string(tid));
        nfo.bypassed = !n.component || n.component->isBypassed();
        nfo.utility = rig::RackGraph::isUtilityOrModulatorNode(tid);
        nfo.structural = rig::RackGraph::isSplitStartNode(tid) || rig::RackGraph::isSplitMidNode(tid) ||
                        rig::RackGraph::isSplitMixNode(tid) || rig::RackGraph::isContainerOrSplitNode(tid);
        if (n.component) nfo.safety = n.component->subBlockSafety(), nfo.mono_pres = n.component->monoPreserving();
        if (rig::RackGraph::isSplitStartNode(tid)) {
            const int id = ++split_ctr;
            ids.push_back(id);
            nfo.lane = cur;
            out.push_back(std::move(nfo));
            stack.push_back(cur + "/S" + std::to_string(id) + "A");
            continue;
        }
        if (rig::RackGraph::isSplitMidNode(tid) && stack.size() > 1) {
            stack.back() = stack[stack.size() - 2] + "/S" + std::to_string(ids.back()) + "B";
            nfo.lane = stack.back();
            out.push_back(std::move(nfo));
            continue;
        }
        if (rig::RackGraph::isSplitMixNode(tid) && stack.size() > 1) {
            stack.pop_back();
            ids.pop_back();
            nfo.lane = stack.back();
            out.push_back(std::move(nfo));
            continue;
        }
        nfo.lane = cur;
        out.push_back(std::move(nfo));
        if (!n.children.empty()) walk(n.children, cur + "/C" + std::to_string(out.size() - 1), out, split_ctr);
    }
}

struct RunStats {
    uint32_t frames = 0;
    double period_us = 0;
    double load_before[3]{}, load_after[3]{};
    std::vector<float> dur_us, late_us;
    int missed = 0, over_budget = 0, skipped_periods = 0, wg_errors = 0;
    rig::RtPool::Stats pool0, pool1;
    bool policy_ok = false, joined = false;
};

void setTimeConstraint(double period_us, double comp_us, double constr_us, bool& ok) {
    thread_time_constraint_policy_data_t p{};
    p.period = static_cast<uint32_t>(usToTicks(period_us));
    p.computation = static_cast<uint32_t>(usToTicks(comp_us));
    p.constraint = static_cast<uint32_t>(usToTicks(constr_us));
    p.preemptible = 1;
    ok = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY, reinterpret_cast<thread_policy_t>(&p),
                           THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
}

double pct(std::vector<float> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(q * double(v.size() - 1) + 0.5));
    return v[i];
}

} // namespace

int main(int argc, char** argv) {
    std::string root, rig_path, di_list, solo_tids, dump_path;
    std::vector<uint32_t> frames_list{64, 128, 256};
    double seconds = 10.0, warmup = 2.0, seg_offset = 2.0;
    bool nopipe = false, solo = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--root") root = next();
        else if (a == "--rig") rig_path = next();
        else if (a == "--di") di_list = next();
        else if (a == "--frames") {
            frames_list.clear();
            for (auto& f : split(next(), ',')) frames_list.push_back(static_cast<uint32_t>(std::atoi(f.c_str())));
        } else if (a == "--seconds") seconds = std::atof(next().c_str());
        else if (a == "--warmup") warmup = std::atof(next().c_str());
        else if (a == "--seg-offset") seg_offset = std::atof(next().c_str());
        else if (a == "--nopipe") nopipe = true;
        else if (a == "--dump") dump_path = next();
        else if (a == "--solo") solo = true;
        else if (a == "--solo-tids") solo = true, solo_tids = next();
        else {
            std::fprintf(stderr, "unknown arg %s\n", a.c_str());
            return 2;
        }
    }
    if (root.empty() || rig_path.empty() || di_list.empty()) {
        std::fprintf(stderr, "usage: preset_rt --root <repo> --rig <file.rig> --di a.wav,b.wav [--frames 64,128,256] [--seconds 10] [--warmup 2] [--nopipe] [--solo]\n");
        return 2;
    }
    {
        mach_timebase_info_data_t tb{};
        mach_timebase_info(&tb);
        g_ticks_per_us = 1000.0 * tb.denom / tb.numer;
    }
    const char* dd = std::getenv("HEADROOM_DATA_DIR");
    if (!dd || !*dd) {
        std::fprintf(stderr, "refusing to run without an isolated HEADROOM_DATA_DIR\n");
        return 2;
    }
    rig::platform::setAppRoot(root);
    // The app tracks Pitch -> MIDI Poly with the neural model (RealtimeAudioServer::start); measure its cost too
    // (HEADROOM_P2M_MODEL=<file> overrides, "-" keeps the classic tracker).
    if (const char* m = std::getenv("HEADROOM_P2M_MODEL")) {
        if (std::string(m) != "-") rig::setNeuralGuitarModelPath(m);
    } else {
        rig::setNeuralGuitarModelPath(root + "/app/resources/models/guitar_midi.hgnn");
    }
    constexpr double kRate = 48000.0;

    // ── signal: the DI tracks, `per` seconds each from seg_offset, warm-up taken from the start of the playlist ──
    const auto tracks = split(di_list, ',');
    const double total_s = warmup + seconds;
    const std::size_t per = static_cast<std::size_t>(std::ceil(total_s / double(tracks.size()) * kRate));
    std::vector<float> sig;
    for (const auto& t : tracks) {
        std::vector<float> d;
        if (!decodeTrack(t, kRate, d)) {
            std::fprintf(stderr, "cannot read DI %s\n", t.c_str());
            return 1;
        }
        const std::size_t off = std::min(d.size(), static_cast<std::size_t>(seg_offset * kRate));
        for (std::size_t k = 0; k < per; ++k) sig.push_back(d[(off + k) % d.size()]);
    }

    // ── the rig, as RealtimeAudioServer::loadResolvedPreset + publishRenderPlanLocked build it ──
    rig::preset::RackPreset preset;
    std::string err;
    if (!rig::preset::loadPreset(rig_path, preset, &err)) {
        std::fprintf(stderr, "preset error: %s\n", err.c_str());
        return 1;
    }
    const auto t_load0 = std::chrono::steady_clock::now();
    rig::RackGraph graph;
    graph.setParallelPrepare(true);
    graph.buildFromPreset(preset, kRate, 512);
    graph.setInputGainLinear(1.0f);
    // (the rig plays at its own knobs' level: there is no preset loudness gain to set)
    auto meters = std::make_shared<rig::UnitMeterBank>();
    graph.setMeterBank(meters);
    graph.syncCabFollow();
    auto plan = graph.renderSnapshot();
    if (nopipe) plan->setPipelining(false);
    const double load_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_load0).count();

    std::vector<NodeInfo> nodes;
    int split_ctr = 0;
    walk((*plan).*rackNodes(), std::string("top"), nodes, split_ctr);

    // ── the audio work interval the caller and the pool's workers share ──
    os_workgroup_interval_t wg = AudioWorkIntervalCreate("preset_rt", OS_CLOCK_MACH_ABSOLUTE_TIME, nullptr);
    auto& pool = rig::RtPool::instance();
    const int slot = pool.acquireWorkgroupSlot();
    if (wg) pool.setWorkgroupPtr(slot, (void*)wg);
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // workers re-join

    std::vector<RunStats> runs;
    std::vector<std::vector<std::vector<float>>> unit_raw; // [run][node][block]
    for (uint32_t F : frames_list) {
        RunStats rs;
        rs.frames = F;
        rs.period_us = 1e6 * F / kRate;
        const std::size_t total_blocks = static_cast<std::size_t>(total_s * kRate / F);
        const std::size_t warm_blocks = static_cast<std::size_t>(warmup * kRate / F);
        const std::size_t meas = total_blocks - warm_blocks;
        rs.dur_us.reserve(meas);
        rs.late_us.reserve(meas);
        for (auto& n : nodes) n.raw.assign(meas, 0.0f), n.prev = *n.cost;
        meters->arm(1e6);
        getloadavg(rs.load_before, 3);
        rs.pool0 = pool.stats();

        std::thread rt([&] {
            pthread_setname_np("preset_rt.audio");
            setTimeConstraint(rs.period_us, 0.75 * rs.period_us, rs.period_us, rs.policy_ok);
            os_workgroup_join_token_s tok{};
            rs.joined = wg && os_workgroup_join(wg, &tok) == 0;
            std::array<float, 1024> in_l{}, in_r{}, out_l{}, out_r{};
            float dc_x = 0, dc_y = 0;
            const uint64_t P = usToTicks(rs.period_us);
            std::size_t pos = 0;
            uint64_t next = mach_absolute_time() + usToTicks(2000);
            for (std::size_t b = 0; b < total_blocks; ++b) {
                mach_wait_until(next);
                const uint64_t t0 = mach_absolute_time();
                if (rs.joined && os_workgroup_interval_start(wg, next, next + P, nullptr) != 0) ++rs.wg_errors;
                // renderChunk: DI player → graph → post (DC blocker / output guard)
                for (uint32_t i = 0; i < F; ++i) {
                    const float s = sig[(pos + i) % sig.size()];
                    in_l[i] = s, in_r[i] = s;
                }
                pos += F;
                const float* ins[2] = {in_l.data(), in_r.data()};
                float* outs[2] = {out_l.data(), out_r.data()};
                rig::AudioBlock block{};
                block.in = ins;
                block.out = outs;
                block.num_channels = 2;
                block.num_frames = F;
                block.sample_rate = kRate;
                plan->process(block);
                for (uint32_t i = 0; i < F; ++i) {
                    const float y = out_l[i] - dc_x + 0.998f * dc_y;
                    dc_x = out_l[i], dc_y = y;
                    out_l[i] = y;
                }
                const uint64_t t1 = mach_absolute_time();
                if (rs.joined && os_workgroup_interval_finish(wg, nullptr) != 0) ++rs.wg_errors;
                if (b >= warm_blocks) {
                    const std::size_t k = b - warm_blocks;
                    const double d = ticksToUs(t1 - t0);
                    rs.dur_us.push_back(static_cast<float>(d));
                    rs.late_us.push_back(static_cast<float>(ticksToUs(t0 > next ? t0 - next : 0)));
                    if (t1 > next + P) ++rs.missed;
                    if (d > rs.period_us) ++rs.over_budget;
                    for (auto& n : nodes) {
                        const double c = *n.cost;
                        n.raw[k] = static_cast<float>(std::max(0.0, (c - 0.8 * n.prev) / 0.2));
                        n.prev = c;
                    }
                } else {
                    for (auto& n : nodes) n.prev = *n.cost;
                }
                next += P;
                const uint64_t now = mach_absolute_time();
                while (next <= now) next += P, ++rs.skipped_periods; // overload: the HAL skips to the next cycle
            }
            if (rs.joined) os_workgroup_leave(wg, &tok);
        });
        rt.join();
        rs.pool1 = pool.stats();
        getloadavg(rs.load_after, 3);
        std::vector<std::vector<float>> ur;
        for (auto& n : nodes) ur.push_back(n.raw);
        if (!dump_path.empty()) { // per-block CSV: frames, block, callback µs, then every timed unit's µs
            if (FILE* f = std::fopen(dump_path.c_str(), runs.empty() ? "w" : "a")) {
                if (runs.empty()) {
                    std::fprintf(f, "frames,block,callback_us");
                    for (std::size_t i = 0; i < nodes.size(); ++i) std::fprintf(f, ",u%zu_%d", i, nodes[i].tid);
                    std::fprintf(f, "\n");
                }
                for (std::size_t k = 0; k < rs.dur_us.size(); ++k) {
                    std::fprintf(f, "%u,%zu,%.2f", rs.frames, k, rs.dur_us[k]);
                    for (auto& v : ur) std::fprintf(f, ",%.2f", v[k]);
                    std::fprintf(f, "\n");
                }
                std::fclose(f);
            }
        }
        unit_raw.push_back(std::move(ur));
        runs.push_back(std::move(rs));
    }

    // ── solo: each unit alone, per-call cost at 16..256 frames ──
    struct Solo {
        std::size_t node;
        double us_per_call[5];
        double a = 0, b = 0;
    };
    std::vector<Solo> solos;
    if (solo) {
        std::set<int32_t> want;
        for (auto& t : split(solo_tids, ',')) want.insert(std::atoi(t.c_str()));
        const uint32_t sizes[5] = {16, 32, 64, 128, 256};
        std::thread th([&] {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            std::vector<float> ol(256), orr(256);
            std::vector<rig::midi::MidiEvent> midi;
            for (std::size_t nfo = 0; nfo < nodes.size(); ++nfo) {
                auto& n = nodes[nfo];
                if (!n.comp || n.bypassed || n.utility || n.structural) continue;
                if (!want.empty() && !want.count(n.tid)) continue;
                Solo so{nfo, {}, 0, 0};
                const std::size_t audio = static_cast<std::size_t>(0.5 * kRate); // per repetition and size
                for (int si = 0; si < 5; ++si) {
                    const uint32_t F = sizes[si];
                    double best = 1e300;
                    for (int rep = 0; rep < 3; ++rep) {
                        std::size_t pos = 0;
                        uint64_t acc = 0;
                        std::size_t calls = 0;
                        for (std::size_t done = 0; done < audio + static_cast<std::size_t>(0.1 * kRate); done += F) {
                            const float* ins[2] = {&sig[pos % (sig.size() - 512)], &sig[pos % (sig.size() - 512)]};
                            float* outs[2] = {ol.data(), orr.data()};
                            rig::AudioBlock blk{};
                            blk.in = ins, blk.out = outs, blk.num_channels = 2, blk.num_frames = F, blk.sample_rate = kRate;
                            blk.midi = &midi;
                            midi.clear();
                            const uint64_t t0 = mach_absolute_time();
                            n.comp->processAudio(blk);
                            const uint64_t t1 = mach_absolute_time();
                            if (done >= static_cast<std::size_t>(0.1 * kRate)) acc += t1 - t0, ++calls;
                            pos += F;
                        }
                        best = std::min(best, ticksToUs(acc) / double(std::max<std::size_t>(1, calls)));
                    }
                    so.us_per_call[si] = best;
                }
                // least squares: us = a + b * frames
                double sx = 0, sy = 0, sxx = 0, sxy = 0;
                for (int si = 0; si < 5; ++si) sx += sizes[si], sy += so.us_per_call[si], sxx += double(sizes[si]) * sizes[si], sxy += sizes[si] * so.us_per_call[si];
                so.b = (5 * sxy - sx * sy) / (5 * sxx - sx * sx);
                so.a = (sy - so.b * sx) / 5;
                solos.push_back(so);
            }
        });
        th.join();
    }

    // ── JSON ──
    std::printf("{\"rig\":\"%s\",\"name\":\"%s\",\"load_s\":%.2f,\"pipe\":%s,\"workers\":%d,\"nodes\":[", jsonEsc(rig_path).c_str(),
                jsonEsc(preset.info.name).c_str(), load_s, nopipe ? "false" : "true", pool.workers());
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& n = nodes[i];
        std::printf("%s{\"i\":%zu,\"tid\":%d,\"name\":\"%s\",\"lane\":\"%s\",\"bypassed\":%s,\"utility\":%s,\"structural\":%s,\"safety\":%d,\"mono\":%s}",
                    i ? "," : "", i, n.tid, jsonEsc(n.name).c_str(), jsonEsc(n.lane).c_str(), n.bypassed ? "true" : "false",
                    n.utility ? "true" : "false", n.structural ? "true" : "false", n.safety, n.mono_pres ? "true" : "false");
    }
    std::printf("],\"runs\":[");
    for (std::size_t r = 0; r < runs.size(); ++r) {
        const auto& rs = runs[r];
        double sum = 0;
        for (float d : rs.dur_us) sum += d;
        const double mean = rs.dur_us.empty() ? 0 : sum / rs.dur_us.size();
        std::printf("%s{\"frames\":%u,\"period_us\":%.2f,\"blocks\":%zu,\"policy_ok\":%s,\"joined\":%s,\"wg_errors\":%d,"
                    "\"load_before\":[%.2f,%.2f,%.2f],\"load_after\":[%.2f,%.2f,%.2f],"
                    "\"mean_pct\":%.2f,\"p50_pct\":%.2f,\"p99_pct\":%.2f,\"p999_pct\":%.2f,\"max_pct\":%.2f,"
                    "\"missed\":%d,\"over_budget\":%d,\"skipped_periods\":%d,\"wake_late_p99_us\":%.1f,\"wake_late_max_us\":%.1f,"
                    "\"pool\":{\"forks\":%llu,\"reclaimed\":%llu,\"refused\":%llu,\"wg_joined\":%d,\"wg_max_parallel\":%d,\"active\":%d},\"units\":[",
                    r ? "," : "", rs.frames, rs.period_us, rs.dur_us.size(), rs.policy_ok ? "true" : "false", rs.joined ? "true" : "false", rs.wg_errors,
                    rs.load_before[0], rs.load_before[1], rs.load_before[2], rs.load_after[0], rs.load_after[1], rs.load_after[2],
                    100 * mean / rs.period_us, 100 * pct(rs.dur_us, 0.5) / rs.period_us, 100 * pct(rs.dur_us, 0.99) / rs.period_us,
                    100 * pct(rs.dur_us, 0.999) / rs.period_us, 100 * pct(rs.dur_us, 1.0) / rs.period_us, rs.missed, rs.over_budget,
                    rs.skipped_periods, pct(rs.late_us, 0.99), pct(rs.late_us, 1.0),
                    (unsigned long long)(rs.pool1.forks - rs.pool0.forks), (unsigned long long)(rs.pool1.reclaimed - rs.pool0.reclaimed),
                    (unsigned long long)(rs.pool1.refused - rs.pool0.refused), rs.pool1.joined_workgroup, rs.pool1.workgroup_max_parallel,
                    rs.pool1.active);
        bool first = true;
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const auto& v = unit_raw[r][i];
            double s = 0;
            for (float x : v) s += x;
            const double m = v.empty() ? 0 : s / v.size();
            if (m <= 0.0) continue;
            std::printf("%s{\"i\":%zu,\"mean_us\":%.2f,\"p99_us\":%.2f,\"max_us\":%.2f}", first ? "" : ",", i, m, pct(v, 0.99), pct(v, 1.0));
            first = false;
        }
        std::printf("]}");
    }
    std::printf("],\"solo\":[");
    for (std::size_t s = 0; s < solos.size(); ++s) {
        const auto& so = solos[s];
        std::printf("%s{\"i\":%zu,\"tid\":%d,\"us_per_call\":[%.2f,%.2f,%.2f,%.2f,%.2f],\"fixed_us\":%.2f,\"per_frame_us\":%.4f}", s ? "," : "",
                    so.node, nodes[so.node].tid, so.us_per_call[0], so.us_per_call[1], so.us_per_call[2], so.us_per_call[3], so.us_per_call[4],
                    so.a, so.b);
    }
    std::printf("]}\n");
    std::fflush(stdout);
    if (wg) {
        pool.releaseWorkgroupSlot(slot);
        os_release(wg);
    }
    std::_Exit(0); // skip teardown (process-wide pool, prepared spares); the OS reclaims everything
}
