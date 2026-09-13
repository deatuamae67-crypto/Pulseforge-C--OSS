-- PulseForge native fallback for the real Psych custom note type
-- "the note". Texture and missHealth are data-driven in the sibling
-- .txt file so they are available even when Lua is disabled.
local dodgeAnimations = {'dodge', 'dodge', 'dodge', 'dodge'}

function goodNoteHit(id, noteData, noteType, isSustainNote)
    if noteType ~= 'the note' then
        return
    end

    local lane = (tonumber(noteData) or 0) + 1
    characterPlayAnim('boyfriend', dodgeAnimations[lane] or 'dodge', true)
    setProperty('boyfriend.specialAnim', true)
    characterPlayAnim('dad', 'Shoot', true)
    setProperty('dad.specialAnim', true)
    playSound('gunshot', 1)
end
