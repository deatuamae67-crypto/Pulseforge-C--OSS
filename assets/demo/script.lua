-- PulseForge's original Lua demo. The exposed API is deliberately smaller and
-- safer than Psych's reflection-based API.

function onCreate()
    debugPrint("Neon Circuit script loaded")
end

function onSongStart()
    debugPrint("Song started at " .. tostring(getSongPosition()) .. " ms")
end

function onBeatHit(beat)
    if beat % 8 == 0 then
        triggerEvent("ScriptPulse", tostring(beat), "")
    end
end

function goodNoteHit(noteId, lane, noteType, isSustain)
    if noteType == "bonus" then
        addScore(50)
    end
end

function noteMiss(noteId, lane, noteType, isSustain)
    -- A host-controlled overlay keeps this bounded and deterministic.
    setHealth(math.max(0.05, getProperty("health")))
end

function onEvent(name, value1, value2)
    if name == "Finale" then
        debugPrint("Finale event received")
    end
end

function onDestroy()
    debugPrint("Neon Circuit script stopped")
end

