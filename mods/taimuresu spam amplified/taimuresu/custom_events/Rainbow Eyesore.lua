-- PulseForge portable compatibility for the Screwed Engine "Rainbow Eyesore" event.
-- value1: duration in milliseconds; value2: pulse speed. The original event
-- relied on a camera GLSL/Haxe filter. PulseForge's SDL renderer implements the
-- same gameplay-visible intent with bounded additive RGB overlays instead of
-- executing arbitrary Haxe/GLSL.

local overlayTags = { 'pfEyesoreRed', 'pfEyesoreGreen', 'pfEyesoreBlue' }
local active = false
local endTimeMs = 0
local pulseSpeed = 2
local phase = 0

local function clamp(value, low, high)
    if value < low then return low end
    if value > high then return high end
    return value
end

local function setOverlayAlpha(index, alpha)
    setProperty(overlayTags[index] .. '.alpha', clamp(alpha, 0, 0.22))
end

local function stopEyesore()
    active = false
    setOverlayAlpha(1, 0)
    setOverlayAlpha(2, 0)
    setOverlayAlpha(3, 0)
end

function onCreate()
    makeLuaSprite(overlayTags[1], '', 0, 0)
    makeGraphic(overlayTags[1], 1280, 720, 'FF0000')
    setObjectCamera(overlayTags[1], 'other')
    setBlendMode(overlayTags[1], 'add')
    addLuaSprite(overlayTags[1], true)

    makeLuaSprite(overlayTags[2], '', 0, 0)
    makeGraphic(overlayTags[2], 1280, 720, '00FF00')
    setObjectCamera(overlayTags[2], 'other')
    setBlendMode(overlayTags[2], 'add')
    addLuaSprite(overlayTags[2], true)

    makeLuaSprite(overlayTags[3], '', 0, 0)
    makeGraphic(overlayTags[3], 1280, 720, '0000FF')
    setObjectCamera(overlayTags[3], 'other')
    setBlendMode(overlayTags[3], 'add')
    addLuaSprite(overlayTags[3], true)

    stopEyesore()
end

function onEvent(name, value1, value2)
    if name ~= 'Rainbow Eyesore' then return end

    local durationMs = tonumber(value1) or 0
    -- Event data is content-controlled. Keep the compatibility effect bounded
    -- even if a malformed chart supplies an extreme duration/speed.
    durationMs = clamp(durationMs, 0, 60000)
    pulseSpeed = clamp(math.abs(tonumber(value2) or 2), 0.25, 16)
    phase = 0

    if durationMs <= 0 then
        stopEyesore()
        return
    end

    endTimeMs = getSongPosition() + durationMs
    active = true
    cameraShake('game', clamp(0.0025 * pulseSpeed, 0.0025, 0.018), durationMs / 1000)
end

function onUpdate(elapsed)
    if not active then return end

    if getSongPosition() >= endTimeMs then
        stopEyesore()
        return
    end

    phase = phase + math.max(0, elapsed) * pulseSpeed * 6.283185307179586
    -- Three phase-shifted positive waves give a bounded RGB-cycling overlay.
    setOverlayAlpha(1, 0.055 + 0.055 * (0.5 + 0.5 * math.sin(phase)))
    setOverlayAlpha(2, 0.055 + 0.055 * (0.5 + 0.5 * math.sin(phase + 2.0943951023931953)))
    setOverlayAlpha(3, 0.055 + 0.055 * (0.5 + 0.5 * math.sin(phase + 4.1887902047863905)))
end

function onDestroy()
    stopEyesore()
end
