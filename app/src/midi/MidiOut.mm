#include "MidiOut.hpp"
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach.h>
#include <atomic>
#include <array>
#include <thread>
#include <chrono>

namespace rig::midi {

struct MidiOut::Impl {
    MIDIClientRef client = 0;
    MIDIEndpointRef source = 0;
    std::array<MidiEvent, 1024> q{};
    std::atomic<uint32_t> w{0}, r{0};
    std::atomic<bool> run{false};
    std::thread th;
    // Idle CPU: the sender thread parks on a Mach semaphore instead of polling every 500 µs (~2000 wakeups/s
    // forever). push() runs on the audio thread: semaphore_signal is a single Mach trap (no locks, no
    // allocation), the usual real-time-safe way to wake a worker from a render callback.
    semaphore_t sem = MACH_PORT_NULL;
    int last_bend = 8192;

    void send(const MidiEvent& e) {
        Byte buf[3];
        int len = 3;
        if (e.type == MidiEvent::NoteOn) {
            buf[0] = 0x90; buf[1] = static_cast<Byte>(e.note & 127);
            buf[2] = static_cast<Byte>(std::clamp(static_cast<int>(e.velocity * 127.0f), 1, 127));
        } else if (e.type == MidiEvent::NoteOff) {
            buf[0] = 0x80; buf[1] = static_cast<Byte>(e.note & 127); buf[2] = 0;
        } else {
            const int v = std::clamp(8192 + static_cast<int>(e.bend / 2.0f * 8191.0f), 0, 16383); // ±2 st default range
            if (v == last_bend) return;
            last_bend = v;
            buf[0] = 0xE0; buf[1] = static_cast<Byte>(v & 127); buf[2] = static_cast<Byte>((v >> 7) & 127);
        }
        Byte pkbuf[64];
        auto* list = reinterpret_cast<MIDIPacketList*>(pkbuf);
        MIDIPacket* pk = MIDIPacketListInit(list);
        pk = MIDIPacketListAdd(list, sizeof(pkbuf), pk, 0, static_cast<ByteCount>(len), buf);
        if (pk) MIDIReceived(source, list);
    }
};

MidiOut::MidiOut() : impl_(std::make_unique<Impl>()) {}
MidiOut::~MidiOut() {
    impl_->run.store(false);
    if (impl_->sem != MACH_PORT_NULL) semaphore_signal(impl_->sem);
    if (impl_->th.joinable()) impl_->th.join();
    if (impl_->sem != MACH_PORT_NULL) semaphore_destroy(mach_task_self(), impl_->sem);
    if (impl_->source) MIDIEndpointDispose(impl_->source);
    if (impl_->client) MIDIClientDispose(impl_->client);
}

bool MidiOut::start() {
    auto& I = *impl_;
    if (I.source) return true;
    if (I.sem == MACH_PORT_NULL && semaphore_create(mach_task_self(), &I.sem, SYNC_POLICY_FIFO, 0) != KERN_SUCCESS) { I.sem = MACH_PORT_NULL; return false; }
    if (MIDIClientCreate(CFSTR("Headroom"), nullptr, nullptr, &I.client) != noErr) return false;
    if (MIDISourceCreate(I.client, CFSTR("Headroom MIDI Out"), &I.source) != noErr) return false;
    I.run.store(true);
    I.th = std::thread([&I] {
        while (I.run.load()) {
            while (I.r.load() != I.w.load()) {
                const uint32_t idx = I.r.load() % I.q.size();
                I.send(I.q[idx]);
                I.r.fetch_add(1);
            }
            semaphore_wait(I.sem);   // woken by push() (or the destructor); extra signals just re-check the queue
        }
    });
    return true;
}

void MidiOut::push(const MidiEvent& e) {
    auto& I = *impl_;
    if (!I.source) return;
    const uint32_t w = I.w.load(std::memory_order_relaxed);
    if (w - I.r.load(std::memory_order_acquire) >= I.q.size()) return;
    I.q[w % I.q.size()] = e;
    I.w.store(w + 1, std::memory_order_release);
    semaphore_signal(I.sem);
}

bool MidiOut::active() const { return impl_->source != 0; }

} // namespace rig::midi

// ───────────────────────────── MIDI in (controllers) ─────────────────────────────
#include "MidiIn.hpp"
#include <mutex>

namespace rig::midi {

struct MidiIn::Impl {
    MIDIClientRef client = 0;
    MIDIPortRef port = 0;
    CcFn fn;
    std::mutex m;
    std::atomic<int> n{0};

    static bool isOwn(MIDIEndpointRef src) {
        CFStringRef name = nullptr;
        if (MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &name) != noErr || !name) return false;
        const bool own = CFStringCompare(name, CFSTR("Headroom MIDI Out"), 0) == kCFCompareEqualTo;
        CFRelease(name);
        return own;
    }
    void connectAll() {
        std::lock_guard<std::mutex> lk(m);
        if (!port) return;
        int c = 0;
        const ItemCount count = MIDIGetNumberOfSources();
        for (ItemCount i = 0; i < count; ++i) {
            const MIDIEndpointRef src = MIDIGetSource(i);
            if (!src || isOwn(src)) continue;
            MIDIPortDisconnectSource(port, src);  // re-connecting a connected source is harmless; this keeps it single
            if (MIDIPortConnectSource(port, src, nullptr) == noErr) ++c;
        }
        n.store(c);
    }
};

MidiIn::MidiIn() : impl_(std::make_unique<Impl>()) {}
MidiIn::~MidiIn() {
    if (impl_->port) MIDIPortDispose(impl_->port);
    if (impl_->client) MIDIClientDispose(impl_->client);
}

bool MidiIn::start(CcFn fn) {
    auto& I = *impl_;
    if (I.port) return true;
    I.fn = std::move(fn);
    Impl* ip = &I;
    if (MIDIClientCreateWithBlock(CFSTR("Headroom Controllers"), &I.client, ^(const MIDINotification* note) {
            if (note && note->messageID == kMIDIMsgSetupChanged) ip->connectAll();   // devices plugged in or out
        }) != noErr) return false;
    if (MIDIInputPortCreateWithProtocol(I.client, CFSTR("Headroom Controllers In"), kMIDIProtocol_1_0, &I.port,
            ^(const MIDIEventList* list, void*) {
                const MIDIEventPacket* p = &list->packet[0];
                for (UInt32 k = 0; k < list->numPackets; ++k) {
                    for (UInt32 w = 0; w < p->wordCount; ++w) {
                        const UInt32 word = p->words[w];
                        if ((word >> 28) != 0x2) continue;              // UMP type 2: MIDI 1.0 channel voice (one word)
                        const int status = static_cast<int>((word >> 16) & 0xFF);
                        if ((status & 0xF0) != 0xB0) continue;          // control change only
                        const int cc = static_cast<int>((word >> 8) & 0x7F), val = static_cast<int>(word & 0x7F);
                        if (cc >= 120) continue;                        // channel-mode messages
                        if (ip->fn) ip->fn(cc, val, (status & 0x0F) + 1);
                    }
                    p = MIDIEventPacketNext(p);
                }
            }) != noErr) return false;
    I.connectAll();
    return true;
}

int MidiIn::sources() const { return impl_->n.load(); }

} // namespace rig::midi
