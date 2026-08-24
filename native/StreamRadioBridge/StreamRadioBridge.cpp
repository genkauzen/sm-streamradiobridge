#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <algorithm>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <iomanip>

// This bridge deliberately uses the Lua 5.1 C ABI exported by the game's
// lua51.dll and the C FMOD ABI exported by the game's fmod.dll. It does not
// touch Scrap Mechanic's private object layouts.

struct lua_State;
using lua_CFunction = int(__cdecl *)(lua_State*);

namespace {

constexpr int LUA_TTABLE = 5;
constexpr int LUA_TFUNCTION = 6;
constexpr int LUA_GLOBALSINDEX = -10002;
constexpr unsigned int FMOD_VERSION = 0x00020207;
constexpr unsigned int FMOD_INIT_3D_RIGHTHANDED = 0x00000004;
constexpr unsigned int FMOD_TIMEUNIT_MS = 0x00000001;
constexpr unsigned int FMOD_3D = 0x00000010;
constexpr unsigned int FMOD_CREATESTREAM = 0x00000080;
constexpr unsigned int FMOD_3D_LINEARSQUAREROLLOFF = 0x00400000;
constexpr float RADIO_MIN_DISTANCE = 1.5f;
// Scrap Mechanic's built-in radio is a short-range source.  Keeping the
// native stream in the same range prevents the old "audible for kilometres"
// behaviour while still allowing a nearby workshop/vehicle to hear it.
constexpr float RADIO_MAX_DISTANCE = 35.0f;
// The bridge owns a separate FMOD system, outside Scrap Mechanic's master
// mixer.  Calibrate its 100% slider position to the game's normal radio level.
// The Lua slider is intentionally calibrated so 1% is the normal radio
// level. Do not apply a second 1% attenuation here.
constexpr float RADIO_OUTPUT_GAIN = 1.0f;
// Scrap Mechanic can briefly suspend interactable fixed updates while a world
// finishes loading or the window loses focus. Three seconds was too short and
// could kill a healthy channel; explicit Lua stop still releases it instantly.
constexpr long long RADIO_STALE_TIMEOUT_SECONDS = 30;

using FmodSystem = void;
using FmodSound = void;
using FmodChannel = void;

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct LuaApi {
    HMODULE module = nullptr;
    int(__cdecl* gettop)(lua_State*) = nullptr;
    void(__cdecl* settop)(lua_State*, int) = nullptr;
    int(__cdecl* getfield)(lua_State*, int, const char*) = nullptr;
    void(__cdecl* setfield)(lua_State*, int, const char*) = nullptr;
    int(__cdecl* getfenv)(lua_State*, int) = nullptr;
    int(__cdecl* setfenv)(lua_State*, int) = nullptr;
    void(__cdecl* createtable)(lua_State*, int, int) = nullptr;
    void(__cdecl* pushcclosure)(lua_State*, lua_CFunction, int) = nullptr;
    void(__cdecl* pushnumber)(lua_State*, double) = nullptr;
    void(__cdecl* pushboolean)(lua_State*, int) = nullptr;
    const char*(__cdecl* pushstring)(lua_State*, const char*) = nullptr;
    int(__cdecl* type)(lua_State*, int) = nullptr;
    double(__cdecl* tonumber)(lua_State*, int) = nullptr;
    int(__cdecl* toboolean)(lua_State*, int) = nullptr;
    const char*(__cdecl* tolstring)(lua_State*, int, size_t*) = nullptr;
    void*(__cdecl* touserdata)(lua_State*, int) = nullptr;
    void(__cdecl* rawset)(lua_State*, int) = nullptr;

    template<typename T>
    bool load(T& target, const char* name) {
        target = reinterpret_cast<T>(GetProcAddress(module, name));
        return target != nullptr;
    }

    bool init() {
        module = GetModuleHandleA("lua51.dll");
        if (!module) module = LoadLibraryA("lua51.dll");
        if (!module) return false;
        return load(gettop, "lua_gettop") && load(settop, "lua_settop") &&
               load(getfield, "lua_getfield") && load(setfield, "lua_setfield") &&
               load(getfenv, "lua_getfenv") && load(setfenv, "lua_setfenv") &&
               load(createtable, "lua_createtable") && load(pushcclosure, "lua_pushcclosure") &&
               load(pushnumber, "lua_pushnumber") && load(pushboolean, "lua_pushboolean") &&
               load(pushstring, "lua_pushstring") && load(type, "lua_type") &&
               load(tonumber, "lua_tonumber") && load(toboolean, "lua_toboolean") &&
               load(tolstring, "lua_tolstring") && load(touserdata, "lua_touserdata") &&
               load(rawset, "lua_rawset");
    }
};

struct FmodApi {
    HMODULE module = nullptr;
    using SystemCreate = int(__cdecl*)(FmodSystem**, unsigned int);
    using SystemInit = int(__cdecl*)(FmodSystem*, int, unsigned int, void*);
    using SystemRelease = int(__cdecl*)(FmodSystem*);
    using SystemUpdate = int(__cdecl*)(FmodSystem*);
    using SystemSet3DSettings = int(__cdecl*)(FmodSystem*, float, float, float);
    using SystemSet3DListener = int(__cdecl*)(FmodSystem*, int, const Vec3*, const Vec3*, const Vec3*, const Vec3*);
    using SystemCreateStream = int(__cdecl*)(FmodSystem*, const char*, unsigned int, void*, FmodSound**);
    using SystemPlaySound = int(__cdecl*)(FmodSystem*, FmodSound*, void*, int, FmodChannel**);
    using SoundRelease = int(__cdecl*)(FmodSound*);
    using SoundGetLength = int(__cdecl*)(FmodSound*, unsigned int*, unsigned int);
    using SoundSetMode = int(__cdecl*)(FmodSound*, unsigned int);
    using ChannelStop = int(__cdecl*)(FmodChannel*);
    using ChannelSetVolume = int(__cdecl*)(FmodChannel*, float);
    using ChannelSet3DAttributes = int(__cdecl*)(FmodChannel*, const Vec3*, const Vec3*);
    using ChannelSet3DMinMaxDistance = int(__cdecl*)(FmodChannel*, float, float);
    using ChannelGetPosition = int(__cdecl*)(FmodChannel*, unsigned int*, unsigned int);
    using ChannelSetPosition = int(__cdecl*)(FmodChannel*, unsigned int, unsigned int);
    using ChannelIsPlaying = int(__cdecl*)(FmodChannel*, int*);

    SystemCreate systemCreate = nullptr;
    SystemInit systemInit = nullptr;
    SystemRelease systemRelease = nullptr;
    SystemUpdate systemUpdate = nullptr;
    SystemSet3DSettings systemSet3DSettings = nullptr;
    SystemSet3DListener systemSet3DListener = nullptr;
    SystemCreateStream systemCreateStream = nullptr;
    SystemPlaySound systemPlaySound = nullptr;
    SoundRelease soundRelease = nullptr;
    SoundGetLength soundGetLength = nullptr;
    SoundSetMode soundSetMode = nullptr;
    ChannelStop channelStop = nullptr;
    ChannelSetVolume channelSetVolume = nullptr;
    ChannelSet3DAttributes channelSet3DAttributes = nullptr;
    ChannelSet3DMinMaxDistance channelSet3DMinMaxDistance = nullptr;
    ChannelGetPosition channelGetPosition = nullptr;
    ChannelSetPosition channelSetPosition = nullptr;
    ChannelIsPlaying channelIsPlaying = nullptr;

    FmodSystem* system = nullptr;

    template<typename T>
    bool load(T& target, const char* name) {
        target = reinterpret_cast<T>(GetProcAddress(module, name));
        return target != nullptr;
    }

    bool init() {
        module = GetModuleHandleA("fmod.dll");
        if (!module) module = LoadLibraryA("fmod.dll");
        if (!module) return false;

        if (!load(systemCreate, "FMOD_System_Create") ||
            !load(systemInit, "FMOD_System_Init") ||
            !load(systemRelease, "FMOD_System_Release") ||
            !load(systemUpdate, "FMOD_System_Update") ||
            !load(systemSet3DSettings, "FMOD_System_Set3DSettings") ||
            !load(systemSet3DListener, "FMOD_System_Set3DListenerAttributes") ||
            !load(systemCreateStream, "FMOD_System_CreateStream") ||
            !load(systemPlaySound, "FMOD_System_PlaySound") ||
            !load(soundRelease, "FMOD_Sound_Release") ||
            !load(soundGetLength, "FMOD_Sound_GetLength") ||
            !load(soundSetMode, "FMOD_Sound_SetMode") ||
            !load(channelStop, "FMOD_Channel_Stop") ||
            !load(channelSetVolume, "FMOD_Channel_SetVolume") ||
            !load(channelSet3DAttributes, "FMOD_Channel_Set3DAttributes") ||
            !load(channelSet3DMinMaxDistance, "FMOD_Channel_Set3DMinMaxDistance") ||
            !load(channelGetPosition, "FMOD_Channel_GetPosition") ||
            !load(channelSetPosition, "FMOD_Channel_SetPosition") ||
            !load(channelIsPlaying, "FMOD_Channel_IsPlaying")) {
            return false;
        }

        if (systemCreate(&system, FMOD_VERSION) != 0 || !system) return false;
        if (systemInit(system, 64, FMOD_INIT_3D_RIGHTHANDED, nullptr) != 0) {
            systemRelease(system);
            system = nullptr;
            return false;
        }
        // Music must keep its original pitch when the player or vehicle moves.
        // Scrap Mechanic uses metres and a right-handed X/Y/Z-up world.
        systemSet3DSettings(system, 0.0f, 1.0f, 1.0f);
        return true;
    }

    void shutdown() {
        if (system) {
            systemRelease(system);
            system = nullptr;
        }
    }
};

struct StreamSession {
    std::atomic<bool> stop{false};
    SOCKET listenSocket = INVALID_SOCKET;
    SOCKET clientSocket = INVALID_SOCKET;
    HANDLE process = nullptr;
    HANDLE stdoutRead = nullptr;
    std::thread worker;
    std::string sourceUrl;
    std::string localUrl;
    float startPosition = 0.0f;

    bool start(const std::string& ffmpegPath, const std::string& source, float position);
    void serve(const std::string& ffmpegPath);
    void shutdown();
    ~StreamSession() { shutdown(); }
};

struct RadioState {
    void* key = nullptr;
    std::string url;
    std::string activeUrl;
    std::string resolvedUrl;
    std::string status = "loading";
    bool playing = false;
    bool loop = false;
    bool resolving = false;
    float volume = 1.0f;
    float position = 0.0f;
    Vec3 radioPosition{};
    Vec3 radioVelocity{};
    Vec3 listenerPosition{};
    Vec3 listenerVelocity{};
    Vec3 listenerForward{0.0f, 1.0f, 0.0f};
    Vec3 listenerUp{0.0f, 0.0f, 1.0f};
    FmodSound* sound = nullptr;
    FmodChannel* channel = nullptr;
    unsigned int durationMs = 0;
    bool resolvedLocal = false;
    std::shared_ptr<StreamSession> stream;
    float streamStartPosition = 0.0f;
    bool ended = false;
    std::uint64_t generation = 0;
    std::uint64_t transportRevision = 0;
    std::uint64_t updateSerial = 0;
    std::chrono::steady_clock::time_point lastUpdate = std::chrono::steady_clock::now();
};

HMODULE g_module = nullptr;
LuaApi g_lua;
FmodApi g_fmod;
std::mutex g_mutex;
std::map<void*, std::shared_ptr<RadioState>> g_radios;
std::map<lua_State*, bool> g_registered;
std::map<lua_State*, bool> g_loggedUpdate;
std::map<lua_State*, bool> g_mirroredSm;
std::map<lua_State*, bool> g_injectedEnvironment;
std::map<lua_State*, bool> g_loggedSetfenv;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_resetRequested{false};
std::atomic<float> g_savedVolume{0.01f};
std::string g_bridgeDir;
FILE* g_log = nullptr;
void** g_pcallSlot = nullptr;
void* g_originalPcall = nullptr;
void** g_setfenvSlot = nullptr;
void* g_originalSetfenv = nullptr;

struct SearchResult {
    std::string url;
    std::string title;
    std::string thumbnail;
};

struct SearchState {
    std::string query;
    std::string status = "Введите название видео";
    bool busy = false;
    bool completed = false;
    std::vector<SearchResult> results;
};

struct PreviewState {
    bool busy = false;
    bool failed = false;
    std::string path;
};

std::mutex g_searchMutex;
SearchState g_search;
std::mutex g_previewMutex;
std::map<std::string, PreviewState> g_previews;

void Log(const char* fmt, ...) {
    if (!g_log) return;
    char buffer[2048]{};
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    fprintf(g_log, "%s\n", buffer);
    fflush(g_log);
}

std::string Trim(const std::string& value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool PatchPointer(void** slot, void* value, void** oldValue = nullptr) {
    if (!slot) return false;
    if (oldValue) *oldValue = *slot;
    DWORD oldProtect = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) return false;
    *slot = value;
    VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));
    return true;
}

bool InstallLuaPcallHook();
int LuaUpdate(lua_State* state);
int LuaStop(lua_State* state);
int LuaReset(lua_State* state);
int LuaStatus(lua_State* state);
int LuaGetVolume(lua_State* state);
int LuaSetVolume(lua_State* state);
int LuaSearch(lua_State* state);
int LuaGetPreviewImage(lua_State* state);
std::string RunCapture(const std::string& command, DWORD timeoutMs, const char* label);
void BeginSearch(const std::string& query);
void BeginPreview(const std::string& url);
void SaveVolume(float volume);

int AbsoluteIndex(lua_State* state, int index) {
    if (index > 0 || index <= LUA_GLOBALSINDEX) return index;
    return g_lua.gettop(state) + index + 1;
}

bool RawSetBridgeOnTable(lua_State* state, int tableIndex) {
    if (!state || !g_lua.rawset || g_lua.type(state, tableIndex) != LUA_TTABLE) return false;
    tableIndex = AbsoluteIndex(state, tableIndex);
    g_lua.pushstring(state, "StreamRadioBridge");
    g_lua.getfield(state, LUA_GLOBALSINDEX, "StreamRadioBridge");
    g_lua.rawset(state, tableIndex);
    return true;
}

void MirrorBridgeToSm(lua_State* state) {
    if (!state || !g_lua.getfield || !g_lua.setfield) return;
    const int top = g_lua.gettop(state);
    g_lua.getfield(state, LUA_GLOBALSINDEX, "sm");
    if (g_lua.type(state, -1) == LUA_TTABLE) {
        if (RawSetBridgeOnTable(state, -1) && !g_mirroredSm[state]) {
            g_mirroredSm[state] = true;
            Log("Lua bridge mirrored to sm state=%p", state);
        }
    }
    g_lua.settop(state, top);
}

void InjectBridgeIntoCallEnvironment(lua_State* state, int nargs) {
    if (!state || !g_lua.getfenv || !g_lua.getfield || !g_lua.setfield) return;
    const int top = g_lua.gettop(state);
    const int functionIndex = top - nargs;
    if (functionIndex < 1 || g_lua.type(state, functionIndex) != LUA_TFUNCTION) return;

    // lua_pcall receives the function at (top - nargs).  Its environment is
    // the sandbox used by Scrap Mechanic mod chunks.  Put the bridge directly
    // into that environment so the mod does not depend on global inheritance.
    if (g_lua.getfenv(state, functionIndex) == 0) {
        g_lua.settop(state, top);
        return;
    }
    if (g_lua.type(state, -1) == LUA_TTABLE) {
        if (RawSetBridgeOnTable(state, -1) && !g_injectedEnvironment[state]) {
            g_injectedEnvironment[state] = true;
            Log("Lua bridge injected into call environment state=%p", state);
        }
    }
    g_lua.settop(state, top);
}

bool EnsureBridgeRegistered(lua_State* state) {
    if (!state || !g_lua.createtable) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    // `sm` can be created after the first pcall in a VM.  Keep retrying the
    // mirror for already-registered states so sandboxed mod chunks can see it.
    if (g_registered[state]) {
        MirrorBridgeToSm(state);
        return true;
    }

    g_lua.createtable(state, 0, 8);
    g_lua.pushcclosure(state, &LuaUpdate, 0);
    g_lua.setfield(state, -2, "update");
    g_lua.pushcclosure(state, &LuaStop, 0);
    g_lua.setfield(state, -2, "stop");
    g_lua.pushcclosure(state, &LuaReset, 0);
    g_lua.setfield(state, -2, "reset");
    g_lua.pushcclosure(state, &LuaStatus, 0);
    g_lua.setfield(state, -2, "status");
    g_lua.pushcclosure(state, &LuaGetVolume, 0);
    g_lua.setfield(state, -2, "getVolume");
    g_lua.pushcclosure(state, &LuaSetVolume, 0);
    g_lua.setfield(state, -2, "setVolume");
    g_lua.pushcclosure(state, &LuaSearch, 0);
    g_lua.setfield(state, -2, "search");
    g_lua.pushcclosure(state, &LuaGetPreviewImage, 0);
    g_lua.setfield(state, -2, "getPreviewImage");
    g_lua.setfield(state, LUA_GLOBALSINDEX, "StreamRadioBridge");

    // Scrap Mechanic runs mod chunks through a sandbox environment.  Mirror
    // the bridge on the shared `sm` table as well.
    MirrorBridgeToSm(state);

    g_registered[state] = true;
    Log("Lua bridge registered state=%p", state);
    return true;
}

template<typename Fn>
void ReadField(lua_State* state, int tableIndex, const char* field, Fn&& fn) {
    const int top = g_lua.gettop(state);
    g_lua.getfield(state, tableIndex, field);
    fn();
    g_lua.settop(state, top);
}

std::string ReadString(lua_State* state, int index) {
    if (g_lua.type(state, index) != 4) return {};
    const char* value = g_lua.tolstring(state, index, nullptr);
    return value ? value : "";
}

float ReadNumber(lua_State* state, int index, float fallback = 0.0f) {
    if (g_lua.type(state, index) != 3) return fallback;
    return static_cast<float>(g_lua.tonumber(state, index));
}

bool ReadBoolean(lua_State* state, int index, bool fallback = false) {
    if (g_lua.type(state, index) != 1) return fallback;
    return g_lua.toboolean(state, index) != 0;
}

void ReadVector(lua_State* state, int index, Vec3& out) {
    if (g_lua.type(state, index) != LUA_TTABLE) return;
    ReadField(state, index, "x", [&]() { out.x = ReadNumber(state, -1, out.x); });
    ReadField(state, index, "y", [&]() { out.y = ReadNumber(state, -1, out.y); });
    ReadField(state, index, "z", [&]() { out.z = ReadNumber(state, -1, out.z); });
}

std::shared_ptr<RadioState> GetRadio(void* key) {
    if (!key) return nullptr;
    auto it = g_radios.find(key);
    if (it != g_radios.end()) return it->second;
    auto radio = std::make_shared<RadioState>();
    radio->key = key;
    g_radios[key] = radio;
    return radio;
}

void PushResult(lua_State* state, const std::shared_ptr<RadioState>& radio) {
    g_lua.createtable(state, 0, 5);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (radio->durationMs > 0) {
        g_lua.pushnumber(state, radio->durationMs / 1000.0);
        g_lua.setfield(state, -2, "duration");
    }
    g_lua.pushstring(state, radio->status.c_str());
    g_lua.setfield(state, -2, "status");
    g_lua.pushboolean(state, radio->channel != nullptr ? 1 : 0);
    g_lua.setfield(state, -2, "ready");
    g_lua.pushboolean(state, radio->ended ? 1 : 0);
    g_lua.setfield(state, -2, "ended");
    g_lua.pushboolean(state, radio->stream != nullptr ? 1 : 0);
    g_lua.setfield(state, -2, "streaming");
}

int LuaUpdate(lua_State* state) {
    if (!state) return 0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_loggedUpdate[state]) {
            g_loggedUpdate[state] = true;
            Log("LuaUpdate entered state=%p top=%d", state, g_lua.gettop(state));
        }
    }
    if (g_lua.gettop(state) < 2) return 0;
    void* key = nullptr;
    std::string url;
    bool playing = false;
    bool loop = false;
    float volume = 1.0f;
    float position = 0.0f;
    std::uint64_t transportRevision = 0;
    Vec3 radioPosition{};
    Vec3 radioVelocity{};
    Vec3 listenerPosition{};
    Vec3 listenerVelocity{};
    Vec3 listenerForward{0.0f, 1.0f, 0.0f};
    Vec3 listenerUp{0.0f, 0.0f, 1.0f};

    ReadField(state, 2, "interactable", [&]() { key = g_lua.touserdata(state, -1); });
    if (!key) return 0;
    ReadField(state, 2, "url", [&]() { url = ReadString(state, -1); });
    ReadField(state, 2, "playing", [&]() { playing = ReadBoolean(state, -1); });
    ReadField(state, 2, "loop", [&]() { loop = ReadBoolean(state, -1); });
    ReadField(state, 2, "volume", [&]() { volume = ReadNumber(state, -1, 1.0f); });
    ReadField(state, 2, "position", [&]() { position = ReadNumber(state, -1); });
    ReadField(state, 2, "revision", [&]() { transportRevision = static_cast<std::uint64_t>(std::max(0.0f, ReadNumber(state, -1))); });
    ReadField(state, 2, "radioPosition", [&]() { ReadVector(state, -1, radioPosition); });
    ReadField(state, 2, "radioVelocity", [&]() { ReadVector(state, -1, radioVelocity); });
    ReadField(state, 2, "listenerPosition", [&]() { ReadVector(state, -1, listenerPosition); });
    ReadField(state, 2, "listenerVelocity", [&]() { ReadVector(state, -1, listenerVelocity); });
    ReadField(state, 2, "listenerForward", [&]() { ReadVector(state, -1, listenerForward); });
    ReadField(state, 2, "listenerUp", [&]() { ReadVector(state, -1, listenerUp); });

    std::shared_ptr<RadioState> radio;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        radio = GetRadio(key);
        if (radio->url != url || radio->transportRevision != transportRevision) {
            // A world transition can reuse the same Lua userdata address.
            // Invalidate any resolver left over from the previous world so a
            // new URL is allowed to start immediately.
            ++radio->generation;
            radio->resolving = false;
            radio->activeUrl.clear();
            radio->ended = false;
        }
        if (radio->url != url || radio->playing != (playing && !url.empty())) {
            Log("LuaUpdate radio=%p playing=%d position=%.2f url=%s", key,
                playing && !url.empty() ? 1 : 0, position, url.substr(0, 180).c_str());
        }
        radio->url = url;
        radio->transportRevision = transportRevision;
        radio->playing = playing && !url.empty();
        if (!radio->playing) radio->ended = false;
        radio->loop = loop;
        radio->volume = std::max(0.0f, std::min(1.0f, volume));
        radio->position = std::max(0.0f, position);
        radio->radioPosition = radioPosition;
        radio->radioVelocity = radioVelocity;
        radio->listenerPosition = listenerPosition;
        radio->listenerVelocity = listenerVelocity;
        radio->listenerForward = listenerForward;
        radio->listenerUp = listenerUp;
        ++radio->updateSerial;
        radio->lastUpdate = std::chrono::steady_clock::now();
        if (url.empty() || !playing) radio->status = "Пауза";
    }

    PushResult(state, radio);
    return 1;
}

int LuaStop(lua_State* state) {
    if (!state || g_lua.gettop(state) < 2) return 0;
    void* key = g_lua.touserdata(state, 2);
    if (!key) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_radios.find(key);
    if (it != g_radios.end()) {
        it->second->playing = false;
        it->second->url.clear();
        ++it->second->generation;
        it->second->resolving = false;
        it->second->activeUrl.clear();
        it->second->status = "Пауза";
    }
    return 0;
}

int LuaReset(lua_State*) {
    // This is a safe in-process reconnect: the audio worker releases every
    // FMOD channel and re-resolves currently playing URLs on its next tick.
    g_resetRequested.store(true);
    return 0;
}

int LuaStatus(lua_State* state) {
    if (!state) return 0;
    g_lua.createtable(state, 0, 2);
    g_lua.pushboolean(state, g_fmod.system != nullptr ? 1 : 0);
    g_lua.setfield(state, -2, "ready");
    g_lua.pushstring(state, g_fmod.system ? "FMOD bridge подключён" : "FMOD bridge не готов");
    g_lua.setfield(state, -2, "status");
    return 1;
}

int LuaGetVolume(lua_State* state) {
    if (!state) return 0;
    g_lua.pushnumber(state, g_savedVolume.load());
    return 1;
}

int LuaSetVolume(lua_State* state) {
    if (!state || g_lua.gettop(state) < 2) return 0;
    const float volume = std::max(0.0f, std::min(1.0f, ReadNumber(state, 2, g_savedVolume.load())));
    g_savedVolume.store(volume);
    SaveVolume(volume);
    g_lua.pushnumber(state, volume);
    return 1;
}

int LuaSearch(lua_State* state) {
    if (!state) return 0;
    std::string query;
    if (g_lua.gettop(state) >= 2) query = Trim(ReadString(state, 2));
    if (query.size() > 160) query.resize(160);
    query.erase(std::remove_if(query.begin(), query.end(), [](unsigned char c) {
        return c == '\r' || c == '\n' || c == '"';
    }), query.end());
    if (!query.empty()) BeginSearch(query);

    g_lua.createtable(state, 0, 18);
    std::lock_guard<std::mutex> lock(g_searchMutex);
    g_lua.pushstring(state, g_search.status.c_str());
    g_lua.setfield(state, -2, "status");
    g_lua.pushboolean(state, g_search.busy ? 1 : 0);
    g_lua.setfield(state, -2, "busy");
    g_lua.pushnumber(state, static_cast<double>(g_search.results.size()));
    g_lua.setfield(state, -2, "count");
    for (size_t i = 0; i < g_search.results.size() && i < 5; ++i) {
        const std::string suffix = std::to_string(i + 1);
        g_lua.pushstring(state, g_search.results[i].url.c_str());
        g_lua.setfield(state, -2, ("url" + suffix).c_str());
        g_lua.pushstring(state, g_search.results[i].title.c_str());
        g_lua.setfield(state, -2, ("title" + suffix).c_str());
        g_lua.pushstring(state, g_search.results[i].thumbnail.c_str());
        g_lua.setfield(state, -2, ("thumbnail" + suffix).c_str());
    }
    return 1;
}

int LuaGetPreviewImage(lua_State* state) {
    if (!state || g_lua.gettop(state) < 2) return 0;
    const std::string url = Trim(ReadString(state, 2));
    if (url.empty() || url.size() > 2048 || url.find('"') != std::string::npos) return 0;
    BeginPreview(url);
    std::lock_guard<std::mutex> lock(g_previewMutex);
    const auto it = g_previews.find(url);
    if (it == g_previews.end() || it->second.path.empty()) return 0;
    std::string path = it->second.path;
    std::replace(path.begin(), path.end(), '\\', '/');
    g_lua.pushstring(state, path.c_str());
    return 1;
}

std::string ModuleDirectory() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(g_module, path, MAX_PATH);
    std::string result(path);
    const auto slash = result.find_last_of("\\/");
    return slash == std::string::npos ? std::string(".") : result.substr(0, slash);
}

std::string ResolveUrl(const std::string& url) {
    const std::string ytDlp = g_bridgeDir + "\\yt-dlp.exe";
    if (GetFileAttributesA(ytDlp.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("yt-dlp.exe is missing: %s", ytDlp.c_str());
        return {};
    }

    char tempName[MAX_PATH]{};
    char tempDir[MAX_PATH]{};
    GetTempPathA(MAX_PATH, tempDir);
    GetTempFileNameA(tempDir, "srb", 0, tempName);
    DeleteFileA(tempName);

    std::string command = "cmd.exe /C \"\"" + ytDlp +
        "\" --no-playlist --no-warnings --no-check-certificates --socket-timeout 15 "
        "--get-url -f \"bestaudio[ext=m4a]/bestaudio[acodec^=mp4a]/bestaudio/best\" \"" +
        url + "\" > \"" + tempName + "\" 2>&1\"";
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    BOOL created = CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) {
        Log("could not start yt-dlp, Win32=%lu", GetLastError());
        DeleteFileA(tempName);
        return {};
    }

    DWORD waitResult = WaitForSingleObject(process.hProcess, 30000);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        Log("yt-dlp timeout for %s", url.c_str());
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    std::ifstream output(tempName, std::ios::binary);
    std::stringstream buffer;
    buffer << output.rdbuf();
    DeleteFileA(tempName);

    std::istringstream lines(buffer.str());
    std::string line;
    while (std::getline(lines, line)) {
        line = Trim(line);
        if (line.empty()) continue;
        if (line.rfind("http://", 0) == 0 || line.rfind("https://", 0) == 0) {
            return line;
        }
    }
    Log("yt-dlp could not resolve %s: %s", url.c_str(), Trim(buffer.str()).c_str());
    return {};
}

bool RunCommand(const std::string& command, DWORD timeoutMs, const char* label) {
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) {
        Log("%s could not start, Win32=%lu", label, GetLastError());
        return false;
    }

    const DWORD waitResult = WaitForSingleObject(process.hProcess, timeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(process.hProcess, 1);
        Log("%s timeout", label);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return false;
    }

    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (exitCode != 0) {
        Log("%s failed exit=%lu", label, exitCode);
        return false;
    }
    return true;
}

std::string MakeTempPath(const char* prefix, const char* suffix) {
    char tempDir[MAX_PATH]{};
    char tempName[MAX_PATH]{};
    GetTempPathA(MAX_PATH, tempDir);
    if (!GetTempFileNameA(tempDir, prefix, 0, tempName)) return {};
    DeleteFileA(tempName);
    return std::string(tempName) + suffix;
}

std::string RunCapture(const std::string& command, DWORD timeoutMs, const char* label) {
    const std::string outputPath = MakeTempPath("src", ".txt");
    if (outputPath.empty()) return {};
    const std::string wrapped = "cmd.exe /D /S /C \"\"" + command + " > \"" + outputPath + "\" 2>&1\"";
    if (!RunCommand(wrapped, timeoutMs, label)) {
        std::ifstream failed(outputPath, std::ios::binary);
        std::stringstream details;
        details << failed.rdbuf();
        Log("%s output: %s", label, Trim(details.str()).substr(0, 600).c_str());
        DeleteFileA(outputPath.c_str());
        return {};
    }
    std::ifstream input(outputPath, std::ios::binary);
    std::stringstream buffer;
    buffer << input.rdbuf();
    DeleteFileA(outputPath.c_str());
    return buffer.str();
}

std::string UserDataDirectory() {
    char localAppData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, MAX_PATH);
    std::string directory = length > 0 && length < MAX_PATH
        ? std::string(localAppData) + "\\StreamRadioBridge"
        : g_bridgeDir + "\\userdata";
    CreateDirectoryA(directory.c_str(), nullptr);
    return directory;
}

void SaveVolume(float volume) {
    const std::string path = UserDataDirectory() + "\\settings.ini";
    const std::string temporary = path + ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        output << "volume=" << std::fixed << std::setprecision(4) << volume << "\n";
    }
    MoveFileExA(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void LoadVolume() {
    std::ifstream input(UserDataDirectory() + "\\settings.ini");
    std::string line;
    while (std::getline(input, line)) {
        if (line.rfind("volume=", 0) != 0) continue;
        try {
            const float value = std::stof(line.substr(7));
            g_savedVolume.store(std::max(0.0f, std::min(1.0f, value)));
        } catch (...) {}
        break;
    }
}

void BeginSearch(const std::string& query) {
    {
        std::lock_guard<std::mutex> lock(g_searchMutex);
        if (g_search.query == query && (g_search.busy || g_search.completed)) return;
        g_search.query = query;
        g_search.status = "Поиск YouTube...";
        g_search.busy = true;
        g_search.completed = false;
        g_search.results.clear();
    }
    std::thread([query]() {
        const std::string ytDlp = g_bridgeDir + "\\yt-dlp.exe";
        const std::string command = "\"" + ytDlp +
            "\" --no-warnings --no-playlist --flat-playlist --skip-download "
            "--print \"%(webpage_url)s::SRB::%(title)s::SRB::%(thumbnail)s\" \"ytsearch5:" + query + "\"";
        const std::string output = RunCapture(command, 45000, "YouTube search");
        std::vector<SearchResult> results;
        std::istringstream lines(output);
        std::string line;
        while (std::getline(lines, line) && results.size() < 5) {
            line = Trim(line);
            const size_t first = line.find("::SRB::");
            const size_t second = first == std::string::npos ? std::string::npos : line.find("::SRB::", first + 7);
            if (first == std::string::npos || second == std::string::npos) continue;
            SearchResult result;
            result.url = line.substr(0, first);
            result.title = line.substr(first + 7, second - first - 7);
            result.thumbnail = line.substr(second + 7);
            if (result.url.rfind("https://", 0) == 0 || result.url.rfind("http://", 0) == 0) {
                results.push_back(std::move(result));
            }
        }
        std::lock_guard<std::mutex> lock(g_searchMutex);
        if (g_search.query != query) return;
        g_search.results = std::move(results);
        g_search.busy = false;
        g_search.completed = true;
        g_search.status = g_search.results.empty() ? "Ничего не найдено" : "Выберите видео";
        Log("YouTube search query=%s results=%zu", query.c_str(), g_search.results.size());
    }).detach();
}

void BeginPreview(const std::string& url) {
    {
        std::lock_guard<std::mutex> lock(g_previewMutex);
        auto& preview = g_previews[url];
        if (preview.busy || preview.failed || !preview.path.empty()) return;
        preview.busy = true;
    }
    std::thread([url]() {
        const std::string ytDlp = g_bridgeDir + "\\yt-dlp.exe";
        const std::string ffmpeg = g_bridgeDir + "\\ffmpeg.exe";
        const std::string thumbnailOutput = RunCapture(
            "\"" + ytDlp + "\" --no-warnings --no-playlist --skip-download --print \"%(thumbnail)s\" \"" + url + "\"",
            30000, "thumbnail resolve");
        std::string thumbnail;
        std::istringstream lines(thumbnailOutput);
        for (std::string line; std::getline(lines, line);) {
            line = Trim(line);
            if (line.rfind("https://", 0) == 0 || line.rfind("http://", 0) == 0) { thumbnail = line; break; }
        }
        std::ostringstream hash;
        hash << std::hex << std::hash<std::string>{}(url);
        const std::string previewDir = UserDataDirectory() + "\\previews";
        CreateDirectoryA(previewDir.c_str(), nullptr);
        const std::string path = previewDir + "\\" + hash.str() + ".jpg";
        bool ok = false;
        if (!thumbnail.empty()) {
            const std::string command = "\"" + ffmpeg +
                "\" -hide_banner -loglevel error -y -i \"" + thumbnail +
                "\" -frames:v 1 -vf \"scale=640:-2\" \"" + path + "\"";
            ok = RunCommand("cmd.exe /D /S /C \"\"" + command + " >nul 2>&1\"", 30000, "thumbnail download");
        }
        LARGE_INTEGER size{};
        HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) { GetFileSizeEx(file, &size); CloseHandle(file); }
        ok = ok && size.QuadPart > 0;
        std::lock_guard<std::mutex> lock(g_previewMutex);
        auto& preview = g_previews[url];
        preview.busy = false;
        preview.failed = !ok;
        preview.path = ok ? path : std::string();
        Log("thumbnail %s url=%s path=%s", ok ? "ready" : "failed", url.c_str(), path.c_str());
    }).detach();
}

bool StreamSession::start(const std::string& ffmpegPath, const std::string& source, float position) {
    sourceUrl = source;
    startPosition = std::max(0.0f, position);
    listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(listenSocket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(listenSocket, 1) == SOCKET_ERROR) {
        closesocket(listenSocket);
        listenSocket = INVALID_SOCKET;
        return false;
    }
    int length = sizeof(address);
    if (getsockname(listenSocket, reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) return false;
    localUrl = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/stream.ogg";
    worker = std::thread([this, ffmpegPath]() { serve(ffmpegPath); });
    return true;
}

void StreamSession::serve(const std::string& ffmpegPath) {
    const char* header = "HTTP/1.0 200 OK\r\nContent-Type: audio/ogg\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    for (int attempt = 0; attempt < 3 && !stop.load(); ++attempt) {
        clientSocket = accept(listenSocket, nullptr, nullptr);
        if (clientSocket == INVALID_SOCKET || stop.load()) return;
        char request[2048]{};
        recv(clientSocket, request, sizeof(request) - 1, 0);
        send(clientSocket, header, static_cast<int>(strlen(header)), 0);
        if (strncmp(request, "HEAD ", 5) != 0) break;
        ::shutdown(clientSocket, SD_BOTH);
        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;
    }
    if (clientSocket == INVALID_SOCKET || stop.load()) return;

    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE stdoutWrite = nullptr;
    if (!CreatePipe(&stdoutRead, &stdoutWrite, &security, 1024 * 1024)) return;
    SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);
    HANDLE nullOutput = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ,
                                    &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    std::ostringstream seek;
    seek << std::fixed << std::setprecision(3) << startPosition;
    const std::string ytDlpPath = g_bridgeDir + "\\yt-dlp.exe";
    // yt-dlp reads and forwards bytes progressively; ffmpeg decodes that pipe
    // to Ogg for FMOD. This avoids both a full-file download and the TLS/media
    // format failures seen when the game's FMOD or ffmpeg opens some signed
    // CDN URLs directly.
    std::string command = "cmd.exe /D /S /C \"\"" + ytDlpPath +
        "\" --no-warnings --no-playlist --no-check-certificates --socket-timeout 20 "
        "-f \"bestaudio/best\" -o - \"" + sourceUrl + "\" 2>nul | \"" + ffmpegPath +
        "\" -hide_banner -loglevel error -i pipe:0 -ss " + seek.str() +
        " -vn -map 0:a:0 -c:a libvorbis -q:a 4 -flush_packets 1 -f ogg pipe:1\"";
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdoutWrite;
    startup.hStdError = nullOutput;
    PROCESS_INFORMATION info{};
    const BOOL created = CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr, g_bridgeDir.c_str(), &startup, &info);
    CloseHandle(stdoutWrite);
    if (nullOutput != INVALID_HANDLE_VALUE) CloseHandle(nullOutput);
    if (!created) {
        Log("ffmpeg stream start failed Win32=%lu", GetLastError());
        if (stdoutRead) { CloseHandle(stdoutRead); stdoutRead = nullptr; }
        if (clientSocket != INVALID_SOCKET) {
            ::shutdown(clientSocket, SD_BOTH);
            closesocket(clientSocket);
            clientSocket = INVALID_SOCKET;
        }
        return;
    }
    CloseHandle(info.hThread);
    process = info.hProcess;
    std::vector<char> buffer(64 * 1024);
    DWORD read = 0;
    while (!stop.load() && ReadFile(stdoutRead, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0) {
        DWORD sentTotal = 0;
        while (!stop.load() && sentTotal < read) {
            const int sent = send(clientSocket, buffer.data() + sentTotal, static_cast<int>(read - sentTotal), 0);
            if (sent <= 0) { stop.store(true); break; }
            sentTotal += static_cast<DWORD>(sent);
        }
    }
    WaitForSingleObject(process, 1000);
    if (!stop.load() && clientSocket != INVALID_SOCKET) {
        ::shutdown(clientSocket, SD_BOTH);
        closesocket(clientSocket);
        clientSocket = INVALID_SOCKET;
    }
}

void StreamSession::shutdown() {
    if (stop.exchange(true) && !worker.joinable()) return;
    if (listenSocket != INVALID_SOCKET) { closesocket(listenSocket); listenSocket = INVALID_SOCKET; }
    if (clientSocket != INVALID_SOCKET) { ::shutdown(clientSocket, SD_BOTH); closesocket(clientSocket); clientSocket = INVALID_SOCKET; }
    if (process) { TerminateProcess(process, 0); }
    if (stdoutRead) { CloseHandle(stdoutRead); stdoutRead = nullptr; }
    if (worker.joinable() && worker.get_id() != std::this_thread::get_id()) worker.join();
    if (process) { CloseHandle(process); process = nullptr; }
}

bool IsLocalAudioPath(const std::string& value) {
    return value.rfind("http://", 0) != 0 && value.rfind("https://", 0) != 0;
}

std::string ResolveAudio(const std::string& url) {
    const std::string ytDlp = g_bridgeDir + "\\yt-dlp.exe";
    const std::string ffmpeg = g_bridgeDir + "\\ffmpeg.exe";
    if (GetFileAttributesA(ytDlp.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("yt-dlp.exe is missing: %s", ytDlp.c_str());
        return ResolveUrl(url);
    }
    if (GetFileAttributesA(ffmpeg.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("ffmpeg.exe is missing: %s; trying direct FMOD URL", ffmpeg.c_str());
        return ResolveUrl(url);
    }
    if (url.find('"') != std::string::npos || url.find('\r') != std::string::npos ||
        url.find('\n') != std::string::npos) {
        Log("refusing unsafe URL");
        return {};
    }

    // Scrap Mechanic's bundled FMOD build cannot open YouTube's AAC/WebM
    // network stream directly (HTTP_ACCESS / FORMAT). Download only the
    // audio track through yt-dlp, transcode it to Ogg Vorbis, and let FMOD
    // play the local OGG. No video is downloaded or rendered.
    const std::string source = MakeTempPath("sra", ".source");
    const std::string output = MakeTempPath("sra", ".ogg");
    if (source.empty() || output.empty()) return ResolveUrl(url);

    const std::string downloadCommand = "cmd.exe /C \"\"" + ytDlp +
        "\" --no-playlist --no-warnings --no-check-certificates --socket-timeout 20 "
        "--no-part --no-continue --force-overwrites --no-progress "
        "-f \"bestaudio[ext=m4a]/bestaudio[acodec^=mp4a]/bestaudio/best\" "
        "-o \"" + source + "\" \"" + url + "\" >nul 2>&1\"";
    const bool downloaded = RunCommand(downloadCommand, 90000, "yt-dlp audio download");
    LARGE_INTEGER sourceSize{};
    const HANDLE sourceHandle = CreateFileA(source.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (sourceHandle != INVALID_HANDLE_VALUE) {
        GetFileSizeEx(sourceHandle, &sourceSize);
        CloseHandle(sourceHandle);
    }

    bool converted = false;
    if (downloaded && sourceSize.QuadPart > 0) {
        const std::string convertCommand = "cmd.exe /C \"\"" + ffmpeg +
            "\" -hide_banner -loglevel error -y -i \"" + source +
            "\" -vn -map 0:a:0 -c:a libvorbis -q:a 5 \"" + output + "\" >nul 2>&1\"";
        converted = RunCommand(convertCommand, 90000, "ffmpeg audio conversion");
    }
    DeleteFileA(source.c_str());

    LARGE_INTEGER outputSize{};
    const HANDLE outputHandle = CreateFileA(output.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (outputHandle != INVALID_HANDLE_VALUE) {
        GetFileSizeEx(outputHandle, &outputSize);
        CloseHandle(outputHandle);
    }
    if (converted && outputSize.QuadPart > 0) {
        Log("audio cached locally: %s", output.c_str());
        return output;
    }

    DeleteFileA(output.c_str());
    Log("audio cache failed for %s; trying direct FMOD URL", url.c_str());
    return ResolveUrl(url);
}

void ReleasePlayback(const std::shared_ptr<RadioState>& radio) {
    if (radio->channel && g_fmod.channelStop) {
        g_fmod.channelStop(radio->channel);
        radio->channel = nullptr;
    }
    if (radio->sound && g_fmod.soundRelease) {
        g_fmod.soundRelease(radio->sound);
        radio->sound = nullptr;
    }
    if (radio->stream) {
        radio->stream->shutdown();
        radio->stream.reset();
    }
    radio->streamStartPosition = 0.0f;
    radio->durationMs = 0;
}

void ReleaseAudio(const std::shared_ptr<RadioState>& radio) {
    ReleasePlayback(radio);
    if (radio->resolvedLocal && !radio->resolvedUrl.empty()) {
        DeleteFileA(radio->resolvedUrl.c_str());
    }
    radio->activeUrl.clear();
    radio->resolvedUrl.clear();
    radio->resolvedLocal = false;
    radio->ended = false;
}

void SetStatus(const std::shared_ptr<RadioState>& radio, const char* status) {
    std::lock_guard<std::mutex> lock(g_mutex);
    radio->status = status;
}

void ProcessRadio(const std::shared_ptr<RadioState>& radio) {
    std::string url;
    std::string resolved;
    bool playing = false;
    float volume = 1.0f;
    float position = 0.0f;
    std::uint64_t generation = 0;
    Vec3 radioPosition{};
    Vec3 radioVelocity{};
    Vec3 listenerPosition{};
    Vec3 listenerVelocity{};
    Vec3 listenerForward{0.0f, 1.0f, 0.0f};
    Vec3 listenerUp{0.0f, 0.0f, 1.0f};
    bool needResolve = false;

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        url = radio->url;
        resolved = radio->resolvedUrl;
        playing = radio->playing;
        volume = radio->volume;
        position = radio->position;
        generation = radio->generation;
        radioPosition = radio->radioPosition;
        radioVelocity = radio->radioVelocity;
        listenerPosition = radio->listenerPosition;
        listenerVelocity = radio->listenerVelocity;
        listenerForward = radio->listenerForward;
        listenerUp = radio->listenerUp;

        if (radio->activeUrl != url) {
            ReleaseAudio(radio);
            radio->activeUrl = url;
            resolved.clear();
            radio->resolving = false;
            radio->status = url.empty() ? "Пауза" : "Загрузка audio...";
        }
        if (playing && !url.empty() && resolved.empty() && !radio->resolving) {
            radio->resolving = true;
            needResolve = true;
        }
    }

    if (!playing || url.empty()) {
        std::lock_guard<std::mutex> lock(g_mutex);
        ReleaseAudio(radio);
        radio->status = "Пауза";
        radio->resolving = false;
        return;
    }

    if (needResolve) {
        std::lock_guard<std::mutex> lock(g_mutex);
        radio->resolving = false;
        if (radio->url != url || radio->generation != generation || !radio->playing) return;
        radio->resolvedUrl = url;
        radio->resolvedLocal = false;
        radio->status = "Буферизация потока...";
        resolved = url;
    }

    if (!radio->channel && !resolved.empty() && g_fmod.system) {
        // Do not use FMOD_NONBLOCKING here.  A non-blocking HTTP stream is not
        // ready when FMOD_System_PlaySound is called, so the old implementation
        // immediately received ERR_NOTREADY, released the stream, and retried
        // forever without ever producing audio.  This worker thread may block
        // while FMOD fills the first network buffer; the game/Lua thread does
        // not block.
        std::shared_ptr<StreamSession> stream;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!radio->stream) {
                auto candidate = std::make_shared<StreamSession>();
                const std::string ffmpeg = g_bridgeDir + "\\ffmpeg.exe";
                if (!candidate->start(ffmpeg, resolved, position)) {
                    radio->status = "Ошибка: локальный поток не создан";
                    return;
                }
                radio->stream = candidate;
                radio->streamStartPosition = position;
            }
            stream = radio->stream;
        }
        FmodSound* sound = nullptr;
        const unsigned int mode = FMOD_3D | FMOD_CREATESTREAM |
            FMOD_3D_LINEARSQUAREROLLOFF;
        const int createResult = g_fmod.systemCreateStream(g_fmod.system, stream->localUrl.c_str(), mode, nullptr, &sound);
        if (createResult != 0 || !sound) {
            Log("FMOD_System_CreateStream failed result=%d url=%s", createResult, stream->localUrl.c_str());
            std::lock_guard<std::mutex> lock(g_mutex);
            if (radio->stream) radio->stream->shutdown();
            radio->stream.reset();
            radio->status = "Ошибка FMOD: поток не создан";
            return;
        }

        const Vec3 noVelocity{};
        g_fmod.systemSet3DListener(g_fmod.system, 0, &listenerPosition, &noVelocity,
                                   &listenerForward, &listenerUp);

        FmodChannel* channel = nullptr;
        const int playResult = g_fmod.systemPlaySound(g_fmod.system, sound, nullptr, 0, &channel);
        if (playResult != 0 || !channel) {
            Log("FMOD_System_PlaySound failed result=%d", playResult);
            g_fmod.soundRelease(sound);
            SetStatus(radio, "Ошибка FMOD: воспроизведение не началось");
            return;
        }

        g_fmod.channelSetVolume(channel, volume * RADIO_OUTPUT_GAIN);
        g_fmod.channelSet3DMinMaxDistance(channel, RADIO_MIN_DISTANCE, RADIO_MAX_DISTANCE);
        g_fmod.channelSet3DAttributes(channel, &radioPosition, &noVelocity);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            radio->sound = sound;
            radio->channel = channel;
            radio->ended = false;
            radio->status = "Играет потоково";
        }
        Log("FMOD streaming audio started radio=%p offset=%.3f volume=%.3f distance=%.3f", radio->key,
            position, volume * RADIO_OUTPUT_GAIN, RADIO_MAX_DISTANCE);
    } else if (radio->channel && g_fmod.channelSet3DAttributes) {
        const Vec3 noVelocity{};
        g_fmod.channelSetVolume(radio->channel, volume * RADIO_OUTPUT_GAIN);
        g_fmod.channelSet3DMinMaxDistance(radio->channel, RADIO_MIN_DISTANCE, RADIO_MAX_DISTANCE);
        g_fmod.channelSet3DAttributes(radio->channel, &radioPosition, &noVelocity);
        g_fmod.systemSet3DListener(g_fmod.system, 0, &listenerPosition, &noVelocity,
                                   &listenerForward, &listenerUp);

        unsigned int currentMs = 0;
        if (g_fmod.channelGetPosition(radio->channel, &currentMs, FMOD_TIMEUNIT_MS) == 0) {
            const float actualPosition = radio->streamStartPosition + static_cast<float>(currentMs) / 1000.0f;
            const float drift = std::fabs(actualPosition - position);
            if (drift > 3.0f) {
                Log("stream resync radio=%p drift=%.3f actual=%.3f target=%.3f", radio->key, drift, actualPosition, position);
                std::lock_guard<std::mutex> lock(g_mutex);
                ReleasePlayback(radio);
                radio->status = "Синхронизация потока...";
                return;
            }
        }
        int isPlaying = 1;
        if (g_fmod.channelIsPlaying(radio->channel, &isPlaying) == 0 && !isPlaying) {
            std::lock_guard<std::mutex> lock(g_mutex);
            ReleasePlayback(radio);
            radio->ended = true;
            radio->status = "Трек завершён";
        }
    }
}

void ResetAudioBridge() {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto& entry : g_radios) {
        const auto& radio = entry.second;
        ReleaseAudio(radio);
        ++radio->generation;
        radio->resolving = false;
        radio->status = radio->playing && !radio->url.empty()
            ? "Переподключение audio..." : "Пауза";
    }
    Log("audio bridge reset requested");
}

void AudioThread() {
    if (!g_fmod.init()) {
        Log("FMOD initialization failed");
        return;
    }
    Log("FMOD bridge initialized");

    while (!g_stop.load()) {
        if (g_resetRequested.exchange(false)) {
            ResetAudioBridge();
        }
        std::vector<std::shared_ptr<RadioState>> radios;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto now = std::chrono::steady_clock::now();
            for (auto it = g_radios.begin(); it != g_radios.end();) {
                const auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second->lastUpdate).count();
                if (age > RADIO_STALE_TIMEOUT_SECONDS) {
                    // A Lua userdata that stops sending updates belongs to an
                    // unloaded world. Release it so its channel cannot keep
                    // playing alongside the next world's radio.
                    Log("stale radio removed radio=%p age=%lld url=%s", it->second->key,
                        static_cast<long long>(age), it->second->url.substr(0, 180).c_str());
                    ReleaseAudio(it->second);
                    it = g_radios.erase(it);
                    continue;
                }
                radios.push_back(it->second);
                ++it;
            }
        }
        for (const auto& radio : radios) ProcessRadio(radio);
        if (g_fmod.system) g_fmod.systemUpdate(g_fmod.system);
        Sleep(25);
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (auto& entry : g_radios) ReleaseAudio(entry.second);
    }
    g_fmod.shutdown();
}

int __cdecl HookedLuaPcall(lua_State* state, int nargs, int nresults, int errfunc) {
    if (state) {
        EnsureBridgeRegistered(state);
        InjectBridgeIntoCallEnvironment(state, nargs);
    }
    using LuaPcall = int(__cdecl*)(lua_State*, int, int, int);
    auto original = reinterpret_cast<LuaPcall>(g_originalPcall);
    return original ? original(state, nargs, nresults, errfunc) : 1;
}

int __cdecl HookedLuaSetfenv(lua_State* state, int index) {
    // lua_setfenv consumes the environment table at the top of the stack.
    // Scrap Mechanic makes mod environments read-only through __newindex, so
    // lua_setfield can appear to succeed while discarding the bridge.  Raw
    // assignment bypasses that protection for this one native API entry.
    if (state && g_lua.gettop(state) > 0 && g_lua.type(state, -1) == LUA_TTABLE) {
        EnsureBridgeRegistered(state);
        if (RawSetBridgeOnTable(state, -1)) {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_loggedSetfenv[state]) {
                g_loggedSetfenv[state] = true;
                Log("Lua bridge raw-injected before setfenv state=%p index=%d", state, index);
            }
        }
    }
    using LuaSetfenv = int(__cdecl*)(lua_State*, int);
    auto original = reinterpret_cast<LuaSetfenv>(g_originalSetfenv);
    return original ? original(state, index) : 0;
}

bool PatchExeLuaImport(const char* symbol, void* replacement, void*** slotOut, void** originalOut) {
    HMODULE exe = GetModuleHandleA(nullptr);
    if (!exe) return false;
    auto base = reinterpret_cast<unsigned char*>(exe);
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!directory.VirtualAddress) return false;
    auto descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + directory.VirtualAddress);

    for (; descriptor->Name; ++descriptor) {
        const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dllName, "lua51.dll") != 0) continue;
        auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base +
            (descriptor->OriginalFirstThunk ? descriptor->OriginalFirstThunk : descriptor->FirstThunk));
        auto addresses = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++addresses) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(import->Name), symbol) != 0) continue;
            void** slot = reinterpret_cast<void**>(&addresses->u1.Function);
            if (!PatchPointer(slot, replacement, originalOut)) return false;
            *slotOut = slot;
            Log("%s hook installed", symbol);
            return true;
        }
    }
    Log("%s import was not found", symbol);
    return false;
}

bool InstallLuaPcallHook() {
    const bool pcallOk = PatchExeLuaImport("lua_pcall", reinterpret_cast<void*>(&HookedLuaPcall),
                                           &g_pcallSlot, &g_originalPcall);
    const bool setfenvOk = PatchExeLuaImport("lua_setfenv", reinterpret_cast<void*>(&HookedLuaSetfenv),
                                             &g_setfenvSlot, &g_originalSetfenv);
    return pcallOk && setfenvOk;
}

DWORD WINAPI StartBridge(LPVOID) {
    g_bridgeDir = ModuleDirectory();
    const std::string logPath = g_bridgeDir + "\\StreamRadioBridge.log";
    g_log = fopen(logPath.c_str(), "a");
    Log("StreamRadioBridge starting");
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        Log("Winsock initialization failed");
        return 0;
    }
    LoadVolume();
    Log("saved local volume=%.4f", g_savedVolume.load());
    if (!g_lua.init()) {
        Log("lua51.dll API initialization failed");
        return 0;
    }
    if (!InstallLuaPcallHook()) return 0;
    std::thread(AudioThread).detach();
    Log("bridge ready; every client must inject this DLL");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        g_module = module;
        HANDLE thread = CreateThread(nullptr, 0, &StartBridge, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
