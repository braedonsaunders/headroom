// MIDI out off macOS (the Mac uses the CoreMIDI virtual source in MidiOut.mm).
//
//   Linux, built with ALSA (RIG_HAVE_ALSA_SEQ): an ALSA sequencer client "Headroom" with a readable port
//     "Headroom MIDI Out" — DAWs, synths and PipeWire's MIDI bridge see it like the CoreMIDI source.
//   Elsewhere (Windows): a stub (active() == false). Windows has no virtual MIDI ports without a driver; the
//     options are Windows MIDI Services' app-to-app loopback endpoints (Windows 11), teVirtualMIDI (needs
//     Tobias Erichsen's licence for redistribution) or asking users to install loopMIDI. docs/PORTING.md.
//
// Events are queued lock-free from the audio thread (push) and sent from a dispatcher thread woken by a POSIX
// semaphore (sem_post is a single syscall with no locks or allocation, the usual way to wake a worker).
#include "MidiOut.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <thread>

#if defined(RIG_HAVE_ALSA_SEQ)
#include <alsa/asoundlib.h>
#include <semaphore.h>
#endif

namespace rig::midi {

#if defined(RIG_HAVE_ALSA_SEQ)
struct MidiOut::Impl {
    snd_seq_t* seq = nullptr;
    int port = -1;
    std::array<MidiEvent, 1024> q{};
    std::atomic<uint32_t> w{0}, r{0};
    std::atomic<bool> run{false};
    std::thread th;
    sem_t sem{};
    bool sem_ok = false;
    int last_bend = 8192;

    void send(const MidiEvent& e) {
        snd_seq_event_t ev;
        snd_seq_ev_clear(&ev);
        snd_seq_ev_set_source(&ev, static_cast<unsigned char>(port));
        snd_seq_ev_set_subs(&ev);
        snd_seq_ev_set_direct(&ev);
        if (e.type == MidiEvent::NoteOn) {
            snd_seq_ev_set_noteon(&ev, 0, e.note & 127, std::clamp(static_cast<int>(e.velocity * 127.0f), 1, 127));
        } else if (e.type == MidiEvent::NoteOff) {
            snd_seq_ev_set_noteoff(&ev, 0, e.note & 127, 0);
        } else {
            const int v = std::clamp(8192 + static_cast<int>(e.bend / 2.0f * 8191.0f), 0, 16383); // ±2 st default range
            if (v == last_bend) return;
            last_bend = v;
            snd_seq_ev_set_pitchbend(&ev, 0, v - 8192);
        }
        snd_seq_event_output_direct(seq, &ev);
    }
};

MidiOut::MidiOut() : impl_(std::make_unique<Impl>()) {}
MidiOut::~MidiOut() {
    auto& I = *impl_;
    I.run.store(false);
    if (I.sem_ok) sem_post(&I.sem);
    if (I.th.joinable()) I.th.join();
    if (I.sem_ok) sem_destroy(&I.sem);
    if (I.seq) snd_seq_close(I.seq);
}

bool MidiOut::start() {
    auto& I = *impl_;
    if (I.seq) return true;
    if (!I.sem_ok) {
        if (sem_init(&I.sem, 0, 0) != 0) return false;
        I.sem_ok = true;
    }
    if (snd_seq_open(&I.seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0) { I.seq = nullptr; return false; }
    snd_seq_set_client_name(I.seq, "Headroom");
    I.port = snd_seq_create_simple_port(I.seq, "Headroom MIDI Out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                        SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    if (I.port < 0) { snd_seq_close(I.seq); I.seq = nullptr; return false; }
    I.run.store(true);
    I.th = std::thread([&I] {
        while (I.run.load()) {
            while (I.r.load() != I.w.load()) {
                I.send(I.q[I.r.load() % I.q.size()]);
                I.r.fetch_add(1);
            }
            while (sem_wait(&I.sem) != 0) {} // EINTR: retry
        }
    });
    return true;
}

void MidiOut::push(const MidiEvent& e) {
    auto& I = *impl_;
    if (!I.seq) return;
    const uint32_t w = I.w.load(std::memory_order_relaxed);
    if (w - I.r.load(std::memory_order_acquire) >= I.q.size()) return;
    I.q[w % I.q.size()] = e;
    I.w.store(w + 1, std::memory_order_release);
    sem_post(&I.sem);
}

bool MidiOut::active() const { return impl_->seq != nullptr; }

#else
struct MidiOut::Impl {};
MidiOut::MidiOut() : impl_(std::make_unique<Impl>()) {}
MidiOut::~MidiOut() = default;
bool MidiOut::start() { return false; }
void MidiOut::push(const MidiEvent&) {}
bool MidiOut::active() const { return false; }
#endif

} // namespace rig::midi

// ───────────────────────────── MIDI in (controllers): not yet off macOS ─────────────────────────────
#include "MidiIn.hpp"

namespace rig::midi {
struct MidiIn::Impl {};
MidiIn::MidiIn() : impl_(std::make_unique<Impl>()) {}
MidiIn::~MidiIn() = default;
bool MidiIn::start(CcFn) { return false; }
int MidiIn::sources() const { return 0; }
} // namespace rig::midi
