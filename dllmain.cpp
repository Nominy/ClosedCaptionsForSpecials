/*****************************************************************
 *  Payday 3  –  "Sound‑Subtitles" Mod (PolyHook² + UE4SS)
 *****************************************************************/

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "MyAwesomeMod.h"
#include <polyhook2/Detour/x64Detour.hpp>
#include <polyhook2/Exceptions/AVehHook.hpp>
#include <polyhook2/Enums.hpp>
#include <LuaMadeSimple/LuaMadeSimple.hpp>
#include <DynamicOutput/DynamicOutput.hpp>
#include <Unreal/UObject.hpp>
#include <Unreal/UObjectGlobals.hpp>
#include <LuaType/LuaUObject.hpp>
#include <lauxlib.h>

using namespace RC;
using namespace RC::Unreal;

// Global variable definitions
PostEventID_t_int g_PostEventTrampoline_intid = nullptr;
PostEventID_t_str g_PostEventTrampoline_strid = nullptr;
std::unordered_set<uint64_t> g_EventFilter;
std::shared_mutex g_FilterMx;
std::unordered_map<AkPlayingID, OriginalCallbackInfo> g_OriginalCallbacks;
std::mutex g_CallbackMutex;
void* g_ModInstance = nullptr;
DWORD g_MainThreadId = 0;
std::queue<SoundEventData> g_PendingSoundEvents;
std::mutex g_EventQueueMutex;

// Helper function implementations
uint64_t hash_wstr(std::wstring_view s) noexcept {
    uint64_t h = 14695981039346656037ULL;
    for (wchar_t c : s) h = (h ^ static_cast<uint64_t>(c)) * 1099511628211ULL;
    return h;
}

bool WantedEvent(uint64_t idOrHash) noexcept {
    std::shared_lock lk(g_FilterMx);
    return g_EventFilter.empty() || g_EventFilter.contains(idOrHash);
}

bool IsInGameThread()
{
    return GetCurrentThreadId() == g_MainThreadId;
}

std::wstring mbs_to_ws(const char* s, size_t len) {
    if (!s || len == 0) return std::wstring();
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, s, static_cast<int>(len), NULL, 0);
    std::wstring wstrTo(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0, s, static_cast<int>(len), &wstrTo[0], size_needed);
    return wstrTo;
}

// Lua C Functions for sound event filter
int l_add_sound_event(lua_State* L) {
    std::unique_lock lk(g_FilterMx);
    if (lua_isinteger(L, 1)) {
        g_EventFilter.insert(static_cast<uint64_t>(lua_tointeger(L, 1)));
    }
    else if (lua_isstring(L, 1)) {
        size_t len;
        const char* s = lua_tolstring(L, 1, &len);
        g_EventFilter.insert(hash_wstr(mbs_to_ws(s, len)));
    }
    return 0;
}

int l_remove_sound_event(lua_State* L) {
    std::unique_lock lk(g_FilterMx);
    if (lua_isinteger(L, 1)) {
        g_EventFilter.erase(static_cast<uint64_t>(lua_tointeger(L, 1)));
    }
    else if (lua_isstring(L, 1)) {
        size_t len;
        const char* s = lua_tolstring(L, 1, &len);
        g_EventFilter.erase(hash_wstr(mbs_to_ws(s, len)));
    }
    return 0;
}

int l_clear_sound_events(lua_State* L) {
    std::unique_lock lk(g_FilterMx);
    g_EventFilter.clear();
    return 0;
}

// Safety Helpers
bool IsProbablyUObjectPtr(uint64_t addr) {
    if (addr < 0x10000) return false;
    if (addr & 0x7) return false;
    if (addr > 0x7FFFFFFF0000ULL) return false;
    return true;
}

bool TryValidateUObject(UObject* obj) {
    if (UObject::IsReal(obj)) {
        __try {
            return obj;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    return false;
}

void PushGameObjectOrNil(LuaMadeSimple::Lua& L, AkGameObjectID id)
{
    if (IsProbablyUObjectPtr(id) && IsInGameThread())
    {
        UObject* obj = reinterpret_cast<UObject*>(id);
        if (TryValidateUObject(obj)) {
            __try {
                LuaType::auto_construct_object(L, obj);
                return;
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {
                Output::send(TEXT("[MyAwesomeMod] Exception while pushing UObject to Lua\n"));
            }
        }
    }
    L.set_nil();
}

// Helper function for safe original callback invocation
bool TryCallOriginalCallback(AkCallbackFunc callback, AkCallbackType in_eType, AkCallbackInfo* in_pCallbackInfo, void* originalCookie) {
    __try {
        void* savedCookie = in_pCallbackInfo->pCookie;
        in_pCallbackInfo->pCookie = originalCookie;
        callback(in_eType, in_pCallbackInfo);
        in_pCallbackInfo->pCookie = savedCookie;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Helper function to safely add flags and inject callback
AkUInt32 ProcessFlagsAndCallback(AkUInt32 originalFlags, AkCallbackFunc originalCallback, void* originalCookie, AkCallbackFunc& outCallback, void*& outCookie)
{
    AkUInt32 newFlags = originalFlags;
    // Only add AK_Duration if not already present
    if (!(originalFlags & AK_Duration)) {
        newFlags |= AK_Duration;
    }
    outCallback = CallbackWrapper;
    outCookie = originalCookie;
    return newFlags;
}

AkPlayingID __cdecl Hook_PostEvent_intid(
    AkUniqueID eventID,
    AkGameObjectID gameObjID,
    AkUInt32 flags,
    AkCallbackFunc cb,
    void* cookie,
    AkUInt32 extCount,
    AkExternalSourceInfo* externals,
    AkPlayingID playingID
)
{
    if (!WantedEvent(eventID))
    {
        return g_PostEventTrampoline_intid(eventID, gameObjID, flags, cb, cookie, extCount, externals, playingID);
    }

    AkCallbackFunc modifiedCb = cb;
    void* modifiedCookie = cookie;
    AkUInt32 modifiedFlags = ProcessFlagsAndCallback(flags, cb, cookie, modifiedCb, modifiedCookie);
    
    AkPlayingID resultPlayingID = g_PostEventTrampoline_intid(
        eventID, gameObjID, modifiedFlags, modifiedCb,
        modifiedCookie, extCount, externals, playingID);
    
    if (resultPlayingID != AK_INVALID_PLAYING_ID) {
        std::wstring eventName = L"EventID_" + std::to_wstring(eventID);
        std::lock_guard<std::mutex> lock(g_CallbackMutex);
        g_OriginalCallbacks[resultPlayingID] = OriginalCallbackInfo(cb, cookie, eventName, eventID, flags);
    }

    return resultPlayingID;
}

AkPlayingID __cdecl Hook_PostEvent_stringid(
    const wchar_t* eventName,
    AkGameObjectID gameObjID,
    AkUInt32 flags,
    AkCallbackFunc cb,
    void* cookie,
    AkUInt32 extCount,
    AkExternalSourceInfo* externals,
    AkPlayingID playingID
)
{
    uint64_t hv = hash_wstr(eventName ? std::wstring_view(eventName) : L"");
    if (!WantedEvent(hv))
    {
        return g_PostEventTrampoline_strid(eventName, gameObjID, flags, cb, cookie, extCount, externals, playingID);
    }

    AkCallbackFunc modifiedCb = cb;
    void* modifiedCookie = cookie;
    AkUInt32 modifiedFlags = ProcessFlagsAndCallback(flags, cb, cookie, modifiedCb, modifiedCookie);
    
    AkPlayingID resultPlayingID = g_PostEventTrampoline_strid(
        eventName, gameObjID, modifiedFlags, modifiedCb,
        modifiedCookie, extCount, externals, playingID);
    
    if (resultPlayingID != AK_INVALID_PLAYING_ID) {
        std::lock_guard<std::mutex> lock(g_CallbackMutex);
        g_OriginalCallbacks[resultPlayingID] = OriginalCallbackInfo(cb, cookie, eventName ? eventName : L"Unknown", hv, flags);
    }

    return resultPlayingID;
}

// Class implementation
PayDay3_SoundSubMod::PayDay3_SoundSubMod()
{
    ModName = TEXT("MyAwesomeMod");
    ModVersion = TEXT("1.0");
    ModDescription = TEXT("Shows subtitles for Wwise cues");
    ModAuthors = TEXT("NaftSan");
    
    g_ModInstance = this;
    g_MainThreadId = 0;  // Will be set in first on_update() call

    std::unique_lock lk(g_FilterMx);
    g_EventFilter.clear();
}

void PayDay3_SoundSubMod::ProcessQueuedSoundEvents()
{
    if (!IsInGameThread() || !m_main_lua) {
        return;
    }

    std::queue<SoundEventData> eventsToProcess;
    {
        std::lock_guard<std::mutex> lock(g_EventQueueMutex);
        eventsToProcess.swap(g_PendingSoundEvents);
    }

    while (!eventsToProcess.empty()) {
        const SoundEventData& eventData = eventsToProcess.front();
        ExecuteSoundHook(eventData.eventName, eventData.mediaID, eventData.playingID, eventData.gameObjectID);
        eventsToProcess.pop();
    }
}

void PayDay3_SoundSubMod::ExecuteSoundHook(const std::wstring& eventName, AkUniqueID mediaID, AkPlayingID playingID, AkGameObjectID gameObjectID)
{
    if (!m_main_lua || !IsInGameThread()) {
        return;
    }

    try {
        if (!m_main_lua->is_global_function("OnSoundCaptured")) {
            return;
        }

        m_main_lua->prepare_function_call("OnSoundCaptured");
        
        std::string eventNameStr(eventName.begin(), eventName.end());
        
        m_main_lua->set_string(eventNameStr);
        m_main_lua->set_integer(static_cast<int64_t>(mediaID));
        m_main_lua->set_integer(static_cast<int64_t>(playingID));

        PushGameObjectOrNil(*m_main_lua, gameObjectID);
        
        m_main_lua->call_function(4, 0);
    }
    catch (...) {
        Output::send(TEXT("[MyAwesomeMod] Lua hook execution failed with exception\n"));
    }
}

void PayDay3_SoundSubMod::QueueSoundEvent(const std::wstring& eventName, AkUniqueID mediaID, AkPlayingID playingID, AkGameObjectID gameObjectID)
{
    std::lock_guard<std::mutex> lock(g_EventQueueMutex);
    g_PendingSoundEvents.push({eventName, mediaID, playingID, gameObjectID});
}

void PayDay3_SoundSubMod::on_unreal_init()
{
    HMODULE hExe = GetModuleHandleW(nullptr);
    if (!hExe)
    {
        Output::send(TEXT("[MyAwesomeMod] ERROR: no module handle\n"));
        return;
    }

    FARPROC pAddr_int = GetProcAddress(hExe, MAKEINTRESOURCEA(171));
    if (!pAddr_int)
    {
        Output::send(TEXT("[MyAwesomeMod] ERROR: PostEvent_int not found\n"));
        return;
    }

    FARPROC pAddr_str = GetProcAddress(hExe, MAKEINTRESOURCEA(173));
    if (!pAddr_str)
    {
        Output::send(TEXT("[MyAwesomeMod] ERROR: PostEvent_str not found\n"));
        return;
    }

    Detour_int = std::make_unique<PLH::x64Detour>(
        reinterpret_cast<uint64_t>(pAddr_int),
        reinterpret_cast<uint64_t>(&Hook_PostEvent_intid),
        reinterpret_cast<uint64_t*>(&g_PostEventTrampoline_intid));

    Detour_str = std::make_unique<PLH::x64Detour>(
        reinterpret_cast<uint64_t>(pAddr_str),
        reinterpret_cast<uint64_t>(&Hook_PostEvent_stringid),
        reinterpret_cast<uint64_t*>(&g_PostEventTrampoline_strid));

    if (!Detour_int->hook() || !Detour_int->isHooked())
    {
        Output::send(TEXT("[MyAwesomeMod] ERROR: detour int failed\n"));
        return;
    }

    if (!Detour_str->hook() || !Detour_str->isHooked())
    {
        Output::send(TEXT("[MyAwesomeMod] ERROR: detour str failed\n"));
        return;
    }
    Output::send(TEXT("[MyAwesomeMod] PostEvent detoured successfully\n"));
}

void PayDay3_SoundSubMod::on_update() 
{
    if (!g_MainThreadId)                // not set yet
        g_MainThreadId = GetCurrentThreadId();

    ProcessQueuedSoundEvents();
}

void PayDay3_SoundSubMod::on_lua_start(LuaMadeSimple::Lua& lua,
                  LuaMadeSimple::Lua& main_lua,
                  LuaMadeSimple::Lua& async_lua,
                  std::vector<LuaMadeSimple::Lua*>& hook_luas)
{
    m_main_lua = &main_lua;

    lua_State* L = m_main_lua->get_lua_state();
    lua_register(L, "AddSoundEvent", l_add_sound_event);
    lua_register(L, "RemoveSoundEvent", l_remove_sound_event);
    lua_register(L, "ClearSoundEvents", l_clear_sound_events);
}

PayDay3_SoundSubMod::~PayDay3_SoundSubMod()
{
    g_ModInstance = nullptr;
    
    if (Detour_int && Detour_int->isHooked())
        Detour_int->unHook();

    if (Detour_str && Detour_str->isHooked())
        Detour_str->unHook();
}

void __cdecl CallbackWrapper(AkCallbackType in_eType, AkCallbackInfo* in_pCallbackInfo)
{
    if (!in_pCallbackInfo) {
        return;
    }

    // Extract playingID using proper bitwise check and safe casting
    AkPlayingID playingID = AK_INVALID_PLAYING_ID;
    if (in_eType & AK_Duration) {
        AkDurationCallbackInfo* info = static_cast<AkDurationCallbackInfo*>(in_pCallbackInfo);
        playingID = info->playingID;
    } else {
        AkEventCallbackInfo* info = static_cast<AkEventCallbackInfo*>(in_pCallbackInfo);
        if (info) {
            playingID = info->playingID;
        }
    }

    // Get original callback info
    OriginalCallbackInfo originalInfo;
    bool foundInfo = false;
    {
        std::lock_guard<std::mutex> lock(g_CallbackMutex);
        auto it = g_OriginalCallbacks.find(playingID);
        if (it != g_OriginalCallbacks.end()) {
            originalInfo = it->second;
            foundInfo = true;
            // Clean up when event ends
            if (in_eType & AK_EndOfEvent) {
                g_OriginalCallbacks.erase(it);
            }
        }
    }
    
    // Our own processing - handle AK_Duration for sound event capture
    if ((in_eType & AK_Duration) && foundInfo && WantedEvent(originalInfo.eventIdentifier))
    {
        AkDurationCallbackInfo* info = static_cast<AkDurationCallbackInfo*>(in_pCallbackInfo);
        if (info->mediaID > 0) {
            static_cast<PayDay3_SoundSubMod*>(g_ModInstance)->QueueSoundEvent(
                originalInfo.eventName, info->mediaID, playingID, in_pCallbackInfo->gameObjID);
        }
    }

    // CRITICAL FIX: Only forward callback types that the original callback requested
    // This prevents the crash by not sending unexpected AK_Duration to game callbacks
    if (foundInfo && originalInfo.originalCallback && (in_eType & originalInfo.originalFlags)) {
        if (!TryCallOriginalCallback(originalInfo.originalCallback, in_eType, in_pCallbackInfo, originalInfo.originalCookie)) {
            Output::send(TEXT("[MyAwesomeMod] Original callback crashed, continuing...\n"));
        }
    }
}

// DLL exports for UE4SS loader
#define MOD_API __declspec(dllexport)
extern "C" {
    MOD_API RC::CppUserModBase* start_mod() { return new PayDay3_SoundSubMod(); }
    MOD_API void uninstall_mod(RC::CppUserModBase* mod) { delete mod; }
}
