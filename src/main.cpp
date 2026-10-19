// Input Precision
// Polls input on a dedicated high-rate thread, timestamps every press/release,
// then splits each frame's update so the input lands on the correct 240 TPS
// physics tick instead of being bunched at the start of the next rendered frame.
// Physics tick rate is NOT changed, so gameplay is not made easier.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace geode::prelude;
using Clock = std::chrono::steady_clock;

struct InputEvent {
    Clock::time_point time;
    bool down;
    int button;     // 1 = jump, 2 = left, 3 = right
    bool player1;
};

struct Binding {
    std::vector<int> keys;
    int button;
    bool player1;
    bool state = false;
};

static std::mutex g_mutex;
static std::vector<InputEvent> g_queue;

static std::atomic<bool> g_enabled{true};
static std::atomic<bool> g_active{false};   // in a level and not paused
static std::atomic<bool> g_mouse{true};
static std::atomic<int>  g_pollRate{1000};
static std::atomic<bool> g_threadStarted{false};
static bool g_injecting = false;            // main thread only

static bool gameFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static bool keyDown(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

static void clearQueue() {
    std::lock_guard lock(g_mutex);
    g_queue.clear();
}

static void pollThread() {
    timeBeginPeriod(1);

    std::vector<Binding> binds = {
        { { VK_SPACE, VK_UP, 'W' }, 1, true },
        { { VK_LBUTTON },           1, true },   // mouse, toggled by setting
        { { VK_LEFT, 'A' },         2, true },
        { { VK_RIGHT, 'D' },        3, true },
    };

    while (true) {
        int rate = std::clamp(g_pollRate.load(), 240, 8000);
        auto interval = std::chrono::nanoseconds(1'000'000'000LL / rate);
        auto next = Clock::now() + interval;

        if (g_enabled && g_active && gameFocused()) {
            auto now = Clock::now();
            for (size_t i = 0; i < binds.size(); ++i) {
                auto& b = binds[i];
                bool isMouse = (i == 1);
                bool pressed = false;
                if (!isMouse || g_mouse) {
                    for (int k : b.keys) {
                        if (keyDown(k)) { pressed = true; break; }
                    }
                }
                if (pressed != b.state) {
                    b.state = pressed;
                    std::lock_guard lock(g_mutex);
                    g_queue.push_back({ now, pressed, b.button, b.player1 });
                }
            }
        } else {
            for (auto& b : binds) b.state = false;
        }

        // Sleep for coarse intervals, spin-yield for very high poll rates.
        if (interval >= std::chrono::milliseconds(2)) {
            std::this_thread::sleep_until(next);
        } else {
            while (Clock::now() < next) std::this_thread::yield();
        }
    }
}

class $modify(IPBaseLayer, GJBaseGameLayer) {
    struct Fields {
        Clock::time_point lastUpdate = Clock::now();
    };

    bool isOurLevel() {
        return static_cast<GJBaseGameLayer*>(PlayLayer::get()) == this;
    }

    // Swallow the game's own (frame-aligned) button events while we inject precise ones.
    void handleButton(bool down, int button, bool isPlayer1) {
        if (!g_injecting && g_enabled && g_active && isOurLevel() && gameFocused()) {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void update(float dt) {
        auto now = Clock::now();
        auto last = m_fields->lastUpdate;
        m_fields->lastUpdate = now;

        if (!g_enabled || !g_active || !isOurLevel()) {
            GJBaseGameLayer::update(dt);
            return;
        }

        std::vector<InputEvent> events;
        {
            std::lock_guard lock(g_mutex);
            events.swap(g_queue);
        }

        if (events.empty()) {
            GJBaseGameLayer::update(dt);
            return;
        }

        double span = std::chrono::duration<double>(now - last).count();
        float consumed = 0.f;

        for (auto const& e : events) {
            double frac = 1.0;
            if (span > 0.0) {
                frac = std::chrono::duration<double>(e.time - last).count() / span;
            }
            frac = std::clamp(frac, 0.0, 1.0);
            float target = static_cast<float>(dt * frac);

            // Advance physics up to the moment the input actually happened...
            if (target > consumed) {
                GJBaseGameLayer::update(target - consumed);
                consumed = target;
            }
            // ...then apply the input there.
            g_injecting = true;
            this->handleButton(e.down, e.button, e.player1);
            g_injecting = false;
        }

        if (dt > consumed) {
            GJBaseGameLayer::update(dt - consumed);
        }
    }
};

class $modify(IPPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;
        clearQueue();
        g_active = true;
        return true;
    }

    void onQuit() {
        g_active = false;
        clearQueue();
        PlayLayer::onQuit();
    }

    void pauseGame(bool unfocused) {
        g_active = false;
        clearQueue();
        PlayLayer::pauseGame(unfocused);
    }

    void resume() {
        PlayLayer::resume();
        clearQueue();
        g_active = true;
    }

    void resetLevel() {
        clearQueue();
        PlayLayer::resetLevel();
    }
};

$on_mod(Loaded) {
    auto mod = Mod::get();
    g_enabled = mod->getSettingValue<bool>("enabled");
    g_mouse = mod->getSettingValue<bool>("mouse");
    g_pollRate = static_cast<int>(mod->getSettingValue<int64_t>("poll-rate"));

    listenForSettingChanges("enabled", [](bool v) { g_enabled = v; clearQueue(); });
    listenForSettingChanges("mouse", [](bool v) { g_mouse = v; });
    listenForSettingChanges("poll-rate", [](int64_t v) { g_pollRate = static_cast<int>(v); });

    if (!g_threadStarted.exchange(true)) {
        std::thread(pollThread).detach();
    }
}
