#pragma once

#include <polyhook2/Detour/x64Detour.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <Mod/CppUserModBase.hpp>
#include <unordered_map>
#include <mutex>
#include <queue>
#include <memory>
#include <shared_mutex>
#include <unordered_set>
#include <string_view>
#include <Windows.h>

// Wwise types
using AkUniqueID = unsigned int;
using AkGameObjectID = unsigned __int64;
using AkUInt32 = unsigned int;
using AkPlayingID = unsigned int;
using AkTimeMs = int;
typedef float AkReal32;

enum AkCallbackType {};

struct AkCallbackInfo
{
    void* pCookie;
    AkGameObjectID gameObjID;
};

struct AkExternalSourceInfo {};

using AkCallbackFunc = void(__cdecl*)(AkCallbackType, AkCallbackInfo*);

struct AkEventCallbackInfo : public AkCallbackInfo
{
    AkPlayingID playingID;
    AkUniqueID eventID;
};

struct AkDurationCallbackInfo : public AkEventCallbackInfo
{
    AkReal32 fDuration;
    AkReal32 fEstimatedDuration;
    AkUniqueID audioNodeID;
    AkUniqueID mediaID;
    bool bStreaming;
};

#define AK_INVALID_PLAYING_ID 0

enum AKRESULT
{
    AK_Success = 1,
    AK_Fail = 2
};

enum AkCallbackFlags
{
    AK_EndOfEvent = 0x0001,
    AK_Duration = 0x0008
};

// Function pointer types
using PostEventID_t_int = AkPlayingID(__cdecl*)(
    AkUniqueID in_eventID,
    AkGameObjectID in_gameObjID,
    AkUInt32 in_uFlags,
    AkCallbackFunc in_pfnCallback,
    void* in_pCookie,
    AkUInt32 in_cExternals,
    AkExternalSourceInfo* in_pExternalSources,
    AkPlayingID in_PlayingID);

using PostEventID_t_str = AkPlayingID(__cdecl*)(
    const wchar_t* in_eventName,
    AkGameObjectID in_gameObjID,
    AkUInt32 in_uFlags,
    AkCallbackFunc in_pfnCallback,
    void* in_pCookie,
    AkUInt32 in_cExternals,
    AkExternalSourceInfo* in_pExternalSources,
    AkPlayingID in_PlayingID);

// Structure to store original callback information
struct OriginalCallbackInfo {
    AkCallbackFunc originalCallback;
    void* originalCookie;
    AkUInt32 originalFlags;
    std::wstring eventName;
    uint64_t eventIdentifier;
    
    OriginalCallbackInfo() : originalCallback(nullptr), originalCookie(nullptr), originalFlags(0), eventIdentifier(0) {}
    OriginalCallbackInfo(AkCallbackFunc cb, void* cookie, const std::wstring& name, uint64_t identifier, AkUInt32 flags)
        : originalCallback(cb), originalCookie(cookie), originalFlags(flags), eventName(name), eventIdentifier(identifier) {}
};

// Structure for queueing sound events from audio thread to game thread
struct SoundEventData {
    std::wstring eventName;
    AkUniqueID mediaID;
    AkPlayingID playingID;
    AkGameObjectID gameObjectID;
};

// Main mod class
class PayDay3_SoundSubMod final : public RC::CppUserModBase
{
private:
    std::unique_ptr<PLH::x64Detour> Detour_int;
    std::unique_ptr<PLH::x64Detour> Detour_str;
    LuaMadeSimple::Lua* m_main_lua = nullptr;

public:
    PayDay3_SoundSubMod();
    ~PayDay3_SoundSubMod() override;

    void ProcessQueuedSoundEvents();
    void ExecuteSoundHook(const std::wstring& eventName, AkUniqueID mediaID, AkPlayingID playingID, AkGameObjectID gameObjectID);
    void QueueSoundEvent(const std::wstring& eventName, AkUniqueID mediaID, AkPlayingID playingID, AkGameObjectID gameObjectID);

    void on_unreal_init() override;
    void on_update() override;
    void on_lua_start(LuaMadeSimple::Lua& lua,
                      LuaMadeSimple::Lua& main_lua,
                      LuaMadeSimple::Lua& async_lua,
                      std::vector<LuaMadeSimple::Lua*>& hook_luas) override;
};

// Function declarations
uint64_t hash_wstr(std::wstring_view s) noexcept;
bool WantedEvent(uint64_t idOrHash) noexcept;
bool IsInGameThread();
std::wstring mbs_to_ws(const char* s, size_t len);
bool IsProbablyUObjectPtr(uint64_t addr);
bool TryValidateUObject(RC::Unreal::UObject* obj);
void PushGameObjectOrNil(LuaMadeSimple::Lua& L, AkGameObjectID id);
bool TryCallOriginalCallback(AkCallbackFunc callback, AkCallbackType in_eType, AkCallbackInfo* in_pCallbackInfo, void* originalCookie);
AkUInt32 ProcessFlagsAndCallback(AkUInt32 originalFlags, AkCallbackFunc originalCallback, void* originalCookie, AkCallbackFunc& outCallback, void*& outCookie);

// Hook functions
AkPlayingID __cdecl Hook_PostEvent_intid(
    AkUniqueID eventID,
    AkGameObjectID gameObjID,
    AkUInt32 flags,
    AkCallbackFunc cb,
    void* cookie,
    AkUInt32 extCount,
    AkExternalSourceInfo* externals,
    AkPlayingID playingID);

AkPlayingID __cdecl Hook_PostEvent_stringid(
    const wchar_t* eventName,
    AkGameObjectID gameObjID,
    AkUInt32 flags,
    AkCallbackFunc cb,
    void* cookie,
    AkUInt32 extCount,
    AkExternalSourceInfo* externals,
    AkPlayingID playingID);

void __cdecl CallbackWrapper(AkCallbackType in_eType, AkCallbackInfo* in_pCallbackInfo);

// Lua C functions
int l_add_sound_event(lua_State* L);
int l_remove_sound_event(lua_State* L);
int l_clear_sound_events(lua_State* L);

// Global variables (extern declarations)
extern PostEventID_t_int g_PostEventTrampoline_intid;
extern PostEventID_t_str g_PostEventTrampoline_strid;
extern std::unordered_set<uint64_t> g_EventFilter;
extern std::shared_mutex g_FilterMx;
extern std::unordered_map<AkPlayingID, OriginalCallbackInfo> g_OriginalCallbacks;
extern std::mutex g_CallbackMutex;
extern void* g_ModInstance;
extern DWORD g_MainThreadId;
extern std::queue<SoundEventData> g_PendingSoundEvents;
extern std::mutex g_EventQueueMutex; 