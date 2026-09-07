function onCreate()
	precacheImage('backgrounds/timeless')
	precacheImage('backgrounds/PurpleCubes')
	precacheImage('backgrounds/FutileBG')
	
	makeLuaSprite('bg1', 'backgrounds/timeless', -1280, -480)
	makeLuaSprite('bg2', 'backgrounds/PurpleCubes', -1280, -480)
	makeLuaSprite('bg3', 'backgrounds/FutileBG', -1280, -480)
	addLuaSprite('bg1')
	addLuaSprite('bg2')
	addLuaSprite('bg3')
	addGlitchEffect('bg1', 2.25, 5, 0.1)
	addGlitchEffect('bg2', 1.75, 4, 0.08)
	addGlitchEffect('bg3', 4, 4.25, 0.12)
	scaleObject('bg1', 2, 2)
	scaleObject('bg2', 1.5, 1.5)
	scaleObject('bg3', 1.5, 1.5)
	setLuaSpriteScrollFactor('bg1', 0.5, 0.5)
	setLuaSpriteScrollFactor('bg2', 0.5, 0.5)
	setLuaSpriteScrollFactor('bg3', 0.5, 0.5)
	
	setProperty('bg2.visible', false)
	setProperty('bg3.visible', false)
end

local bgPhase = 0

function onBeatHit()
	if curBeat >= 624 * 4 and bgPhase == 0 then
		setProperty('bg1.visible', false)
		setProperty('bg2.visible', true)
		bgPhase = bgPhase + 1
	end
	if curBeat >= 1008 * 4 and bgPhase == 1 then
		setProperty('bg2.visible', false)
		setProperty('bg3.visible', true)
		bgPhase = bgPhase + 1
	end
	if curBeat >= 1680 * 4 and bgPhase == 2 then
		setProperty('bg3.visible', false)
		setProperty('bg1.visible', true)
		addGlitchEffect('bg1', 4 * math.pi, 2, 0.7)
		bgPhase = bgPhase + 1
	end
end