local UEHelpers          = require("UEHelpers")
local utils              = require("utils")
local transcriptions     = require("transcriptions")

local GetPlayerController = UEHelpers.GetPlayerController

local TextRenderActorClass = StaticFindObject("/Game/Mods/MyAwesomeMod/UI/Widgets/Misc/ClosedCaptionActor.ClosedCaptionActor_C")
local WBP_ClosedCaptionOverheadClass = StaticFindObject("/Game/Mods/MyAwesomeMod/UI/Widgets/Misc/WBP_ClosedCaptionOverhead.WBP_ClosedCaptionOverhead_C")
local textBlockClass = StaticFindObject("/Game/Mods/MyAwesomeMod/UI/Widgets/Misc/WBP_ClosedCaptionOverhead.WBP_ClosedCaptionOverhead_C:WidgetTree.TextBlock_38")
local font = StaticFindObject("/Game/UI/Assets/Fonts/Din_Pro/DINPro-CondMedium_Font.DINPro-CondMedium_Font")


local Z_OFFSET = 120
local SHORT_SOUND_THRESHOLD = 2.0 -- Sounds shorter than 2 seconds are considered "short"

local activeTextActors = {}

local function getGameObjectKey(gameObject)
    if not gameObject or not gameObject:IsValid() then
        return nil
    end
    return tostring(gameObject:GetFullName())
end

local function cleanupTextActors()
    for key, textActor in pairs(activeTextActors) do
        if not textActor or not textActor:IsValid() or textActor.IsShuttingDown then
            activeTextActors[key] = nil
        end
    end
end

local function shouldIgnoreShortSound(gameObject, newDuration)
    local key = getGameObjectKey(gameObject)
    if not key then return false end
    
    local existingActor = activeTextActors[key]
    if existingActor and existingActor:IsValid() and not existingActor.IsShuttingDown then
        local remainingTime = existingActor.Lifetime - existingActor.CurrentLife
        -- If new sound is short (< 2 seconds) and there's still time left on current sound, ignore it
        if newDuration < SHORT_SOUND_THRESHOLD and remainingTime > 0 then
            return true
        end
    end
    
    return false
end

local function handleExistingTextActor(gameObject, newText, newDuration)
    local key = getGameObjectKey(gameObject)
    if not key then return false end
    
    local existingActor = activeTextActors[key]
    if existingActor and existingActor:IsValid() and not existingActor.IsShuttingDown then
        local remainingTime = existingActor.Lifetime - existingActor.CurrentLife
        
        -- If new sound is longer than remaining time, replace the current text
        if newDuration > remainingTime then
            existingActor:SetMyText(newText)
            existingActor.Lifetime = existingActor.CurrentLife + newDuration
            return true
        else
            -- If new sound is shorter or equal, keep the existing one (don't replace)
            return true
        end
    end
    
    return false
end



local function spawnText(world, gameObject, msg, duration, attachToActor, zOffset)
    if not TextRenderActorClass:GetFullName() then
        TextRenderActorClass = StaticFindObject("/Game/Mods/MyAwesomeMod/UI/Widgets/Misc/ClosedCaptionActor.ClosedCaptionActor_C")
    end

    if not WBP_ClosedCaptionOverheadClass:GetFullName() then
        WBP_ClosedCaptionOverheadClass = StaticFindObject("/Game/Mods/MyAwesomeMod/UI/Widgets/Misc/WBP_ClosedCaptionOverhead.WBP_ClosedCaptionOverhead_C")
    end

    if not textBlockClass:GetFullName() then
        textBlockClass = StaticFindObject("/Game/Mods/MyAwesomeMod/UI/Widgets/Misc/WBP_ClosedCaptionOverhead.WBP_ClosedCaptionOverhead_C:WidgetTree.TextBlock_38")
    end

    if not font:GetFullName() then
        font = StaticFindObject("/Game/UI/Assets/Fonts/Din_Pro/DINPro-CondMedium_Font.DINPro-CondMedium_Font")
    end
    textBlockClass.Font.FontObject = font
    
    cleanupTextActors()
    
    -- Check if we should ignore this short sound
    if shouldIgnoreShortSound(gameObject, duration) then
        return
    end
    
    if handleExistingTextActor(gameObject, msg, duration) then
        return
    end
    
    local key = getGameObjectKey(gameObject)
    if key and activeTextActors[key] then
        local existingActor = activeTextActors[key]
        if existingActor and existingActor:IsValid() then
            existingActor:BeginShutdown()
        end
        activeTextActors[key] = nil
    end
    
    local loc = gameObject:K2_GetActorLocation()
    loc.Z = loc.Z + (zOffset or Z_OFFSET)
    local rot = gameObject:K2_GetActorRotation()

    local textActor = world:SpawnActor(TextRenderActorClass, loc, rot)
    if not textActor or not textActor:IsValid() then return end
    
    textActor:SetMyText(msg)
    textActor.Lifetime = duration
    
    if key then
        activeTextActors[key] = textActor
    end

    if attachToActor then
        textActor:K2_AttachToActor(gameObject, NAME_None, 1, 1, 1, false)
    end
end


ClearSoundEvents()

AddSoundEvent("bulldozer")
AddSoundEvent("cloaker")
AddSoundEvent("drone")
AddSoundEvent("grenadier")
AddSoundEvent("shield")
AddSoundEvent("techie")
AddSoundEvent("taser")

AddSoundEvent("fbi")

AddSoundEvent("star_assemble")
AddSoundEvent("star_camera")
AddSoundEvent("star_comment_loud")
AddSoundEvent("star_comment_stealth")
AddSoundEvent("star_comment")
AddSoundEvent("star_distract")
AddSoundEvent("star_elevator")
AddSoundEvent("star_end")
AddSoundEvent("star_escape")
AddSoundEvent("star_escort")
AddSoundEvent("star_far")
AddSoundEvent("star_gang_loud")
AddSoundEvent("star_gang")
AddSoundEvent("star_greed")
AddSoundEvent("star_guard")
AddSoundEvent("star_hands")
AddSoundEvent("star_hang")
AddSoundEvent("star_helicopter")
AddSoundEvent("star_lance")
AddSoundEvent("star_metal")
AddSoundEvent("star_open")
AddSoundEvent("star_postpone")
AddSoundEvent("star_react")
AddSoundEvent("star_van")
AddSoundEvent("star_wait")
AddSoundEvent("star_walk")
AddSoundEvent("star_winch")


OnSoundCaptured = function(eventName, mediaID, playingID, gameObject)
    local key = tostring(mediaID)
    if gameObject and gameObject:IsValid() and transcriptions[key] then
        local t = transcriptions[key]
        if eventName == "fbi" then
            spawnText(gameObject:GetWorld(), gameObject:GetOwner(), t.text, t.duration, true, 280)
        else
            spawnText(gameObject:GetWorld(), gameObject:GetOwner(), t.text, t.duration, true)
        end
    end
    print(eventName, mediaID, playingID, gameObject)
end

OnF4Pressed = function()
    local pc = GetPlayerController()
    local pawn = pc.Pawn
    spawnText(pawn:GetWorld(), pawn, "uwu", 10, false)
end


-- Periodic cleanup to maintain performance
local lastCleanupTime = 0
local CLEANUP_INTERVAL = 5.0 -- Clean up every 5 seconds

local function periodicCleanup()
    local currentTime = os.clock()
    if currentTime - lastCleanupTime >= CLEANUP_INTERVAL then
        cleanupTextActors()
        lastCleanupTime = currentTime
    end
end

-- Hook into game tick for periodic cleanup
NotifyOnNewObject("/Script/Engine.GameViewportClient", function(self)
    if self:GetFullName():find("GameViewportClient") then
        self.GetHook = self.GetHook or {}
        if not self.GetHook.PeriodicCleanup then
            self.GetHook.PeriodicCleanup = RegisterHook("/Script/Engine.GameViewportClient:Tick", function()
                periodicCleanup()
            end)
        end
    end
end)

RegisterKeyBind(Key.F4, OnF4Pressed)