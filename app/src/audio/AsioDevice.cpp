// Standalone Windows ASIO host. Only the explicitly opened driver is activated.
#if defined(_WIN32) && RIG_WITH_ASIO
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "AsioDevice.hpp"
#include "../platform/Denormals.hpp"
#include <windows.h>
#include <objbase.h>
#include <common/asiosys.h>
#include <common/asio.h>
#include <common/iasiodrv.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace rig::audio {
namespace {

std::string utf8(const wchar_t* text, int length) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
    std::string result(n, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, text, length, result.data(), n, nullptr, nullptr);
    return result;
}

struct DriverEntry {
    std::string name;
    CLSID clsid;
};

struct RegistryKey {
    HKEY key = nullptr;
    ~RegistryKey() { if (key) RegCloseKey(key); }
};

std::vector<DriverEntry> installed() {
    RegistryKey root;
    // Default registry view has the bitness of this host.
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ, &root.key) != ERROR_SUCCESS)
        return {};
    std::vector<DriverEntry> entries;
    for (DWORD i = 0;; ++i) {
        wchar_t name[256]{};
        DWORD length = 256;
        if (RegEnumKeyExW(root.key, i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        wchar_t guid[64]{};
        DWORD bytes = sizeof(guid);
        if (RegGetValueW(root.key, name, L"CLSID", RRF_RT_REG_SZ, nullptr, guid, &bytes) != ERROR_SUCCESS) continue;
        CLSID clsid{};
        if (SUCCEEDED(CLSIDFromString(guid, &clsid))) entries.push_back({utf8(name, length), clsid});
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return entries;
}

void check(ASIOError code, const char* operation) {
    if (code != ASE_OK) throw std::runtime_error(std::string(operation) + " (ASIO " + std::to_string(code) + ")");
}

struct Format {
    unsigned bytes, bits;
    bool floating, little;
};

Format formatFor(ASIOSampleType type) {
    const bool little = type >= ASIOSTInt16LSB;
    switch (type - (little ? ASIOSTInt16LSB : 0)) {
        case ASIOSTInt16MSB: return {2, 16, false, little};
        case ASIOSTInt24MSB: return {3, 24, false, little};
        case ASIOSTInt32MSB: return {4, 32, false, little};
        case ASIOSTFloat32MSB: return {4, 32, true, little};
        case ASIOSTFloat64MSB: return {8, 64, true, little};
        case ASIOSTInt32MSB16: return {4, 16, false, little};
        case ASIOSTInt32MSB18: return {4, 18, false, little};
        case ASIOSTInt32MSB20: return {4, 20, false, little};
        case ASIOSTInt32MSB24: return {4, 24, false, little};
        default: throw std::runtime_error("Unsupported ASIO sample format " + std::to_string(type));
    }
}

float readSample(const uint8_t* p, Format f) {
    uint64_t word = 0;
    for (unsigned i = 0; i < f.bytes; ++i)
        word = (word << 8) | p[f.little ? f.bytes - 1 - i : i];
    if (f.floating) {
        if (f.bytes == 8) {
            double sample;
            std::memcpy(&sample, &word, 8);
            return static_cast<float>(sample);
        }
        const auto bits = static_cast<uint32_t>(word);
        float sample;
        std::memcpy(&sample, &bits, 4);
        return sample;
    }
    // Reduced-width PCM in a 32-bit container is right justified.
    const int64_t sign = int64_t{1} << (f.bits - 1);
    const int64_t value = static_cast<int64_t>(word) & (2 * sign - 1);
    return static_cast<float>(((value ^ sign) - sign) / static_cast<double>(sign));
}

void writeSample(uint8_t* p, Format f, float sample) {
    uint64_t word = 0;
    if (!std::isfinite(sample)) sample = 0;
    if (f.floating && f.bytes == 8) {
        const double value = sample;
        std::memcpy(&word, &value, 8);
    } else if (f.floating) {
        uint32_t bits;
        std::memcpy(&bits, &sample, 4);
        word = bits;
    } else {
        const int64_t scale = int64_t{1} << (f.bits - 1);
        const double value = std::clamp<double>(sample, -1., 1.) * scale;
        word = static_cast<uint64_t>(std::min<int64_t>(std::llround(value), scale - 1));
    }
    for (unsigned i = 0; i < f.bytes; ++i) {
        p[f.little ? i : f.bytes - 1 - i] = static_cast<uint8_t>(word);
        word >>= 8;
    }
}

long selectFrames(long wanted, long lo, long hi, long preferred, long step) {
    if (lo <= 0 || hi < lo || preferred < lo || preferred > hi || step < -1)
        throw std::runtime_error("Invalid buffer constraints from ASIO driver");
    if (wanted < lo || wanted > hi || step == 0) return preferred;
    if (step > 0) return (wanted - lo) % step == 0 ? wanted : preferred;
    for (int64_t n = lo; n <= hi; n *= 2)
        if (n == wanted) return wanted;
    return preferred;
}
} // namespace

struct AsioDevice::Impl {
    static std::atomic<Impl*> target;
    enum Request : unsigned { Reset = 1, Resync = 2, Latencies = 4, ClockChange = 8, Panel = 16, Resize = 32 };
    std::atomic<unsigned> requests{0};
    std::atomic<bool> stopping{false};
    std::atomic<long> newBufferSize{0};
    std::thread apartment;
    mutable std::mutex stateMutex;
    std::string name, lastError;
    int availableInputs = 0;

    AudioSettings settings;
    bool live = false;
    RenderFn render;
    StatusFn publish;
    std::atomic<uint64_t>* overloads = nullptr;
    DriverEntry entry{};
    HWND window = nullptr;
    IASIO* driver = nullptr;
    bool created = false, started = false, outputReady = false;
    Status status;
    ASIOCallbacks callbacks{bufferSwitch, rateChange, message, timeSwitch};
    std::array<ASIOBufferInfo, 4> buffers{};
    std::array<Format, 4> formats{};
    int inputs = 0, outputs = 0;
    std::array<std::vector<float>, 4> planes;
    // Set only after all descriptors and scratch buffers are ready for the driver.
    std::atomic<bool> ready{false};

    static void bufferSwitch(long index, ASIOBool) {
        auto* self = target.load();
        if (self && self->ready.load() && (index == 0 || index == 1)) self->process(index);
    }
    static ASIOTime* timeSwitch(ASIOTime*, long index, ASIOBool direct) {
        bufferSwitch(index, direct);
        return nullptr;
    }
    static void rateChange(ASIOSampleRate rate) {
        auto* self = target.load();
        if (self && self->ready.load() && !(std::abs(self->status.rate - rate) < 0.5))
            self->requests.fetch_or(ClockChange);
    }
    static long message(long selector, long value, void*, double*) {
        auto* self = target.load();
        if (!self) return 0;
        if (selector == kAsioSelectorSupported) {
            switch (value) {
                case kAsioEngineVersion: case kAsioResetRequest: case kAsioResyncRequest: case kAsioBufferSizeChange:
                case kAsioLatenciesChanged: case kAsioSupportsTimeInfo: case kAsioOverload: return 1;
                default: return 0;
            }
        }
        switch (selector) {
            case kAsioEngineVersion: return 2;
            case kAsioSupportsTimeInfo: return 1;
            case kAsioBufferSizeChange:
                if (value <= 0) return 0;
                self->newBufferSize.store(value);
                self->requests.fetch_or(Resize);
                return 1;
            case kAsioOverload: ++*self->overloads; return 1;
            case kAsioResyncRequest: ++*self->overloads; self->requests.fetch_or(Resync); return 1;
            case kAsioResetRequest: self->requests.fetch_or(Reset); return 1;
            case kAsioLatenciesChanged: self->requests.fetch_or(Latencies); return 1;
            default: return 0;
        }
    }
    void process(long half) {
        rig::platform::ScopedNoDenormals ftz;
        const int n = status.frames;
        for (int ch = 0; ch < inputs; ++ch) {
            const auto f = formats[ch];
            auto* p = static_cast<const uint8_t*>(buffers[ch].buffers[half]);
            for (int i = 0; i < n; ++i, p += f.bytes) planes[ch][i] = readSample(p, f);
        }
        if (render) render(planes[0].data(), planes[inputs == 2 ? 1 : 0].data(),
                           planes[2].data(), planes[3].data(), n);
        for (int ch = 0; ch < outputs; ++ch) {
            const auto f = formats[inputs + ch];
            auto* p = static_cast<uint8_t*>(buffers[inputs + ch].buffers[half]);
            for (int i = 0; i < n; ++i, p += f.bytes) writeSample(p, f, planes[2 + ch][i]);
        }
        // This is the one IASIO operation designed to run inside bufferSwitch.
        if (outputReady) driver->outputReady();
    }

    void latency() {
        long in = 0, out = 0;
        check(driver->getLatencies(&in, &out), "Reading ASIO latency");
        status.latencyMs = 1000.0 * (double(out) + (live ? double(in) : 0.0)) / status.rate;
    }

    void open(double streamRate) {
        HRESULT hr = CoCreateInstance(entry.clsid, nullptr, CLSCTX_INPROC_SERVER, entry.clsid,
                                      reinterpret_cast<void**>(&driver));
        if (FAILED(hr)) throw std::runtime_error("Creating ASIO driver (HRESULT " + std::to_string(hr) + ")");
        if (driver->init(window) == ASIOFalse) throw std::runtime_error("Driver initialization failed");
        double rate = 0;
        const bool known = driver->getSampleRate(&rate) == ASE_OK && std::isfinite(rate) && rate > 0;
        // Initial open negotiates the requested rate. After a reset, respect the
        // external clock and let the server reprepare its graph if it changed.
        if (!streamRate && std::isfinite(settings.sample_rate) && settings.sample_rate > 0 &&
            (!known || std::abs(settings.sample_rate - rate) > 0.5) &&
            driver->canSampleRate(settings.sample_rate) == ASE_OK)
            driver->setSampleRate(settings.sample_rate);
        check(driver->getSampleRate(&rate), "Reading ASIO sample rate");
        if (!std::isfinite(rate) || rate <= 0) throw std::runtime_error("Driver has no active sample clock");
        if (streamRate && std::abs(rate - streamRate) > 0.5)
            throw std::runtime_error("ASIO clock changed. Restart audio to use the new sample rate");
        status.rate = rate;

        long ins = 0, outs = 0;
        check(driver->getChannels(&ins, &outs), "Reading ASIO channels");
        if (outs <= 0 || ins < 0) throw std::runtime_error("No usable ASIO output");
        if (live && (settings.input_channel < 0 || settings.input_channel >= ins))
            throw std::runtime_error("Selected ASIO input channel is unavailable");
        status.inputs = ins;
        inputs = !live ? 0 : (settings.input_mode == "stereo" && settings.input_channel < ins - 1 ? 2 : 1);
        outputs = std::min<long>(2, outs);

        long lo, hi, preferred, step;
        check(driver->getBufferSize(&lo, &hi, &preferred, &step), "Reading ASIO buffer sizes");
        status.frames = selectFrames(settings.buffer_size, lo, hi, preferred, step);
        for (int ch = 0; ch < inputs + outputs; ++ch) {
            ASIOChannelInfo info{};
            info.isInput = ch < inputs ? ASIOTrue : ASIOFalse;
            info.channel = ch < inputs ? settings.input_channel + ch : ch - inputs;
            check(driver->getChannelInfo(&info), "Reading ASIO channel format");
            buffers[ch] = {info.isInput, info.channel, {nullptr, nullptr}};
            formats[ch] = formatFor(info.type);
        }
        auto create = [&] { return driver->createBuffers(buffers.data(), inputs + outputs, status.frames, &callbacks); };
        ASIOError result = create();
        if (result != ASE_OK && status.frames != preferred) {
            status.frames = preferred;
            result = create();
        }
        check(result, "Creating ASIO buffers");
        created = true;
        for (auto& plane : planes) plane.assign(status.frames, 0);
        for (int ch = 0; ch < inputs + outputs; ++ch) {
            for (void* p : buffers[ch].buffers) {
                if (!p) throw std::runtime_error("Driver returned an empty ASIO buffer");
                if (ch >= inputs) std::memset(p, 0, size_t(status.frames) * formats[ch].bytes);
            }
        }
        outputReady = driver->outputReady() == ASE_OK;
        latency();
        ready.store(true);
        check(driver->start(), "Starting ASIO audio");
        started = true;
        status.running = true;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            name = entry.name;
            availableInputs = ins;
            lastError.clear();
        }
        publish(status);
    }

    void close() {
        if (started) driver->stop();
        ready.store(false);
        if (created) driver->disposeBuffers();
        if (driver) driver->Release();
        driver = nullptr;
        created = started = false;
        status.running = false;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            name.clear();
            availableInputs = 0;
        }
        publish(status);
    }

    static BOOL CALLBACK closePanelWindow(HWND hwnd, LPARAM owner) {
        if (hwnd != reinterpret_cast<HWND>(owner) && IsWindowVisible(hwnd))
            PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return TRUE;
    }
    static void CALLBACK panelTimer(HWND hwnd, UINT, UINT_PTR, DWORD) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (self && self->stopping.load())
            EnumThreadWindows(GetCurrentThreadId(), closePanelWindow, reinterpret_cast<LPARAM>(hwnd));
    }
    void showPanel() {
        // A modal driver panel runs a nested Windows message loop. The owner
        // timer can dismiss it even while controlPanel() is on the stack.
        const auto timer = SetTimer(window, 1, 100, panelTimer);
        const auto result = timer ? driver->controlPanel() : ASE_HWMalfunction;
        if (timer) KillTimer(window, timer);
        std::lock_guard<std::mutex> lock(stateMutex);
        lastError = result == ASE_OK ? "" : "ASIO driver control panel failed (" + std::to_string(result) + ")";
    }

    void run(std::promise<bool> startup) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        bool claimed = false, signaled = false;
        try {
            if (FAILED(hr)) throw std::runtime_error("Could not initialize the ASIO COM apartment");
            auto list = installed();
            auto it = std::find_if(list.begin(), list.end(), [&](const auto& d) {
                return settings.asio_driver.empty() || settings.asio_driver == d.name;
            });
            if (it == list.end()) throw std::runtime_error("Selected ASIO driver is not installed");
            entry = *it;
            Impl* vacant = nullptr;
            claimed = target.compare_exchange_strong(vacant, this);
            if (!claimed) throw std::runtime_error("Another ASIO stream is open");
            window = CreateWindowExW(0, L"STATIC", L"Headroom ASIO", WS_POPUP,
                                     0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!window) throw std::runtime_error("Could not create the ASIO host window");
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
            open(0);
            startup.set_value(true);
            signaled = true;
            while (!stopping.load()) {
                MSG msg;
                while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                    TranslateMessage(&msg);
                    DispatchMessageW(&msg);
                }
                const auto work = requests.exchange(0);
                if (work & ClockChange)
                    throw std::runtime_error("ASIO clock changed. Restart audio to use the new sample rate");
                if (work & (Reset | Resync | Resize)) {
                    if (work & Resize) settings.buffer_size = newBufferSize.load();
                    const double oldRate = status.rate;
                    close();
                    if (stopping.load()) break;
                    open(oldRate);
                } else if (work & Latencies) {
                    latency();
                    publish(status);
                }
                if ((work & Panel) && !stopping.load()) showPanel();
                // Pump COM/window messages between deferred driver requests. No
                // system calls, allocations or driver reconfiguration in asioMessage.
                MsgWaitForMultipleObjects(0, nullptr, FALSE, 25, QS_ALLINPUT);
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(stateMutex);
            lastError = "ASIO " + settings.asio_driver + ": " + e.what();
        }
        close();
        if (claimed) target.store(nullptr);
        if (window) DestroyWindow(window);
        window = nullptr;
        if (SUCCEEDED(hr)) CoUninitialize();
        if (!signaled) startup.set_value(false);
    }
};

std::atomic<AsioDevice::Impl*> AsioDevice::Impl::target{nullptr};
AsioDevice::AsioDevice() : impl_(std::make_unique<Impl>()) {}
AsioDevice::~AsioDevice() { stop(); }
std::vector<std::string> AsioDevice::drivers() {
    std::vector<std::string> result;
    for (auto& d : installed()) result.push_back(std::move(d.name));
    return result;
}
bool AsioDevice::start(const AudioSettings& settings, bool live, RenderFn render, StatusFn status,
                       std::atomic<uint64_t>& overloads, std::string* err) {
    stop();
    auto& s = *impl_;
    s.settings = settings;
    s.live = live;
    s.render = std::move(render);
    s.publish = std::move(status);
    s.overloads = &overloads;
    s.requests.store(0);
    s.stopping.store(false);
    s.status = {};
    std::promise<bool> startup;
    auto result = startup.get_future();
    s.apartment = std::thread(&Impl::run, &s, std::move(startup));
    if (result.get()) return true;
    s.apartment.join();
    if (err) *err = error();
    return false;
}
void AsioDevice::stop() {
    auto& s = *impl_;
    s.stopping.store(true);
    if (s.apartment.joinable()) s.apartment.join();
    std::lock_guard<std::mutex> lock(s.stateMutex);
    s.lastError.clear();
}
std::string AsioDevice::activeName() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->name;
}
std::string AsioDevice::error() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->lastError;
}
int AsioDevice::inputChannels() const {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    return impl_->availableInputs;
}
bool AsioDevice::openPanel(std::string* err) {
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    if (impl_->name.empty() || impl_->stopping.load()) {
        if (err) *err = "Start an ASIO driver to open its control panel";
        return false;
    }
    impl_->requests.fetch_or(Impl::Panel);
    return true;
}
} // namespace rig::audio
#endif
