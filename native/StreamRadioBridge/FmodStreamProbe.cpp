#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>
#include <cstring>

struct Vec3 { float x, y, z; };
using System = void;
using Sound = void;
using Channel = void;

using SystemCreate = int(__cdecl*)(System**, unsigned int);
using SystemInit = int(__cdecl*)(System*, int, unsigned int, void*);
using SystemRelease = int(__cdecl*)(System*);
using SystemUpdate = int(__cdecl*)(System*);
using CreateStream = int(__cdecl*)(System*, const char*, unsigned int, void*, Sound**);
using PlaySound = int(__cdecl*)(System*, Sound*, void*, int, Channel**);
using SoundRelease = int(__cdecl*)(Sound*);
using ChannelSetVolume = int(__cdecl*)(Channel*, float);
using ChannelStop = int(__cdecl*)(Channel*);

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: FmodStreamProbe.exe <fmod.dll> <direct-audio-url>\n");
        return 2;
    }

    HMODULE module = LoadLibraryA(argv[1]);
    if (!module) {
        std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
        return 3;
    }

    auto get = [&](const char* name) { return GetProcAddress(module, name); };
    auto systemCreate = reinterpret_cast<SystemCreate>(get("FMOD_System_Create"));
    auto systemInit = reinterpret_cast<SystemInit>(get("FMOD_System_Init"));
    auto systemRelease = reinterpret_cast<SystemRelease>(get("FMOD_System_Release"));
    auto systemUpdate = reinterpret_cast<SystemUpdate>(get("FMOD_System_Update"));
    auto createStream = reinterpret_cast<CreateStream>(get("FMOD_System_CreateStream"));
    auto playSound = reinterpret_cast<PlaySound>(get("FMOD_System_PlaySound"));
    auto soundRelease = reinterpret_cast<SoundRelease>(get("FMOD_Sound_Release"));
    auto channelSetVolume = reinterpret_cast<ChannelSetVolume>(get("FMOD_Channel_SetVolume"));
    auto channelStop = reinterpret_cast<ChannelStop>(get("FMOD_Channel_Stop"));
    if (!systemCreate || !systemInit || !systemRelease || !systemUpdate ||
        !createStream || !playSound || !soundRelease || !channelSetVolume || !channelStop) {
        std::fprintf(stderr, "required FMOD exports are missing\n");
        return 4;
    }

    System* system = nullptr;
    int result = systemCreate(&system, 0x00020207);
    std::printf("System_Create=%d\n", result);
    if (result != 0 || !system) return 5;
    result = systemInit(system, 64, 0, nullptr);
    std::printf("System_Init=%d\n", result);
    if (result != 0) {
        systemRelease(system);
        return 6;
    }

    // 0x10 = FMOD_3D, 0x80 = FMOD_CREATESTREAM.
    Sound* sound = nullptr;
    result = createStream(system, argv[2], 0x10 | 0x80, nullptr, &sound);
    std::printf("CreateStream=%d sound=%p\n", result, sound);
    if (result != 0 || !sound) {
        systemRelease(system);
        return 7;
    }

    Channel* channel = nullptr;
    result = playSound(system, sound, nullptr, 0, &channel);
    std::printf("PlaySound=%d channel=%p\n", result, channel);
    if (channel) channelSetVolume(channel, 0.0f);
    for (int i = 0; i < 80 && channel; ++i) {
        systemUpdate(system);
        Sleep(25);
    }

    if (channel) channelStop(channel);
    soundRelease(sound);
    systemRelease(system);
    return result == 0 ? 0 : 8;
}
