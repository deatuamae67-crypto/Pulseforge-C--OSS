luaDebugMode = true

local dadX = 0
local dadY = 0
local rndX = 0
local rndY = 0
local strumPosX = {}
local strumPosY = {}
local phase = 0
local strumMovement = 0
local allowMovement = false
local offset = 56
local pi = math.pi

local shakeRangeX = 0
local shakeRangeY = 0

function linear(s, e, t)
    return (1 - t) * s + t * e
end

function onCreate()
	makeLuaSprite('fader0', nil)
	makeLuaSprite('fader1', nil)
	makeGraphic('fader0', 1280, 720, '000000')
	makeGraphic('fader1', 1280, 720, 'FFFFFF')
	setScrollFactor('fader0')
	setScrollFactor('fader1')
	addLuaSprite('fader0', true)
	addLuaSprite('fader1', true)
	setObjectCamera('fader0', 'camOther')
	setObjectCamera('fader1', 'camOther')
	setProperty('fader1.alpha', 0)
	setProperty('camHUD.alpha', 0)
	setProperty('camHUD.zoom', 5)
	
	dadX = getProperty('dad.x')
	dadY = getProperty('dad.y')
	
	rndX = math.random() + 0.5
	rndY = math.random() + 0.5
end

function onCreatePost()
	for i = 0,7 do
		x = getPropertyFromGroup('strumLineNotes', i, 'x')
		y = getPropertyFromGroup('strumLineNotes', i, 'y')
		table.insert(strumPosX, x)
		table.insert(strumPosY, y)
	end
end

function onSongStart()
	doTweenAlpha('twn', 'fader0', 0, 16 * 240 / 250, 'quadout')
end

function onBeatHit()
	if curBeat >= 64 and phase == 0 then
		setProperty('camHUD.alpha', 1)
		phase = phase + 1
	end

	-- thearchy fade in & out
	if curBeat >= 592 * 4 and phase == 1 then
		noteTweenEnd()
		setProperty('fader0.alpha', 0)
		phase = phase + 1
	end
	if curBeat >= 608 * 4 and phase == 2 then
		cancelTween('twn')
		doTweenAlpha('twn1', 'fader1', 1, 4 * 240 / 250, 'quartin')
		phase = phase + 1
	end
	if curBeat >= 624 * 4 and phase == 3 then
		cancelTween('twn1')
		doTweenAlpha('twn2', 'fader1', 0, 1 * 240 / 250, 'quintout')
		allowMovement = true
		phase = phase + 1
		strumMovement = strumMovement + 1
	end
	
	-- phonophobia fade in & out (the song is tsukareta wtf)
	if curBeat >= 944 * 4 and phase == 4 then
		cancelTween('twn2')
		doTweenAlpha('twn1', 'fader0', 1, 16 * 240 / 250, 'quadout')
		noteTweenEnd()
		phase = phase + 1
	end
	if curBeat >= 992 * 4 and phase == 5 then
		cancelTween('twn1')
		doTweenAlpha('twn2', 'fader1', 1, 4 * 240 / 250, 'quartin')
		phase = phase + 1
	end
	if curBeat >= 1008 * 4 and phase == 6 then
		cancelTween('twn2')
		doTweenAlpha('twn3', 'fader1', 0, 1 * 240 / 250, 'quintout')
		doTweenAlpha('twn4', 'fader0', 0, 0.001)
		allowMovement = true
		phase = phase + 1
		strumMovement = strumMovement + 1
	end
	
	-- hellbreaker fade in & out (the song is thearchy wtf)
	if curBeat >= 1648 * 4 and phase == 7 then
		allowMovement = false
		noteTweenEnd()
		phase = phase + 1
	end
	if curBeat >= 1664 * 4 and phase == 8 then
		cancelTween('twn3')
		cancelTween('twn4')
		doTweenAlpha('twn1', 'fader1', 1, 4 * 240 / 250, 'quartin')
		phase = phase + 1
	end
	if curBeat >= 1680 * 4 and phase == 9 then
		cancelTween('twn1')
		doTweenAlpha('twn2', 'fader1', 0, 1 * 240 / 250, 'quintout')
		allowMovement = true
		phase = phase + 1
		strumMovement = strumMovement + 1
	end
	
	-- byebye
	if curBeat >= 2128 * 4 and phase == 10 then
		cancelTween('twn2')
		doTweenAlpha('twn1', 'fader0', 1, 16 * 240 / 250, 'cubein')
		doTweenAlpha('twn3', 'camHUD', 0, 8 * 240 / 250, 'quadin')
		allowMovement = false
		noteTweenEnd()
		phase = phase + 1
	end
	
	if strumMovement == 0 and (curBeat % (curBpm / bpm) == 0) and curBeat < 592 * 4 then
		noteTween1(curBeat / (curBpm / bpm))
	end
end

function onUpdate()
	local songPos = getSongPosition() / 1000
	local syncedBeat = songPos * curBpm / 240
	
	if strumMovement == 0 then
		setProperty('dad.angle', math.cos(syncedBeat * rndX))
		setProperty('dad.x', dadX + 4 * math.cos(syncedBeat * rndX))
		setProperty('dad.y', dadY + 8 * math.sin(syncedBeat * rndY))
	elseif strumMovement == 1 then
		if allowMovement then
			noteTween2(curDecBeat / 4)
		end
		local therachyX = 9
		local therachyY = 5
		local therachyAngleSpeed = 0.1875
		local therachyAngleDepth = 540
		setProperty('dad.x', dadX + 4 * math.cos(syncedBeat * rndX) * therachyX)
		setProperty('dad.y', dadY + 8 * math.sin(syncedBeat * rndY) * therachyY)
		setProperty('dad.angle', math.cos(syncedBeat * rndX * therachyAngleSpeed) * therachyAngleDepth)
	elseif strumMovement == 2 then
		if allowMovement then
			noteTween3(curDecBeat / 4)
		end
		-- t >= 0 and (t > 1 and 1 or t) or 0
		local t = (curDecBeat - 1072 * 4) / 256
		local t3 = t >= 0 and (t > 1 and 1 or t*t*t) or 0
		shakeRange = linear(0, 100, t3)
		setProperty('dad.x', dadX + math.random() * shakeRange)
		setProperty('dad.y', dadY + math.random() * shakeRange)
	else
		if allowMovement then
			noteTween4(curDecBeat / 4)
		end
	end
	
	if not mustHitSection then
		cameraSetTarget("dad")
	end
end

function onUpdatePost()
	local singing = string.find(callMethod('dad.getAnimationName'), 'sing') ~= nil
	local skipDance = getProperty('gf.skipDance')
	if getProperty('daHit') then
		if not skipDance then
			playAnim('gf', 'scared', true)
			setProperty('gf.skipDance', true)
		end
	elseif getProperty('bfHit') and mustHitSection or not singing then
		if skipDance then
			setProperty('gf.skipDance', false)
		end
	end
end

function noteTweenEnd()
	local oMiddle = (strumPosX[4] - strumPosX[1]) / 2 + strumPosX[1]
	local bMiddle = (strumPosX[8] - strumPosX[5]) / 2 + strumPosX[5]
	allowMovement = false
	for i = 0,7 do
		noteTweenX("x"..i, i, strumPosX[i+1], 3.84, "quartOut")
		noteTweenY("y"..i, i, strumPosY[i+1], 3.84, "quartOut")
	end
end

function noteTween1(beat)
	local oMiddle = (strumPosX[4] - strumPosX[1]) / 2 + strumPosX[1]
	local bMiddle = (strumPosX[8] - strumPosX[5]) / 2 + strumPosX[5]
	for i = 0,7 do
		if i < 4 then
		noteTweenX("x"..i, i, linear(oMiddle, strumPosX[i+1], beat % 4 < 2 and 0 or 2), 0.48, "quadinOut")
		else
		noteTweenX("x"..i, i, linear(bMiddle, strumPosX[i+1], beat % 4 < 2 and 2 or 0), 0.48, "quadinOut")
		end
	end
end

function noteTween2(time)
	local nSpd = pi
	local nScl = 100
	local nRot = time / 16
	for i = 0,7 do
		if i == 4 then
			nRot = nRot + 0.5
		end
		setPropertyFromGroup("strumLineNotes", i, "x", strumPosX[i+1] + math.sin(time * nSpd + (i - 3.5) * 0.5 * pi) * math.cos(nRot * pi) * nScl)
		setPropertyFromGroup("strumLineNotes", i, "y", strumPosY[i+1] + math.sin(time * nSpd + (i - 3.5) * 0.5 * pi) * math.sin(nRot * pi) * nScl)
	end
end

function noteTween3(time)
	local timeDeg = time * pi
	for i = 0,7 do
		local nowX = 0
		local nowY = 0
		if i < 4 then
			nowX = screenWidth / 2 - offset + math.sin(timeDeg + i*pi/2) * 300
			nowY = screenHeight / 2 - offset + math.cos(timeDeg + i*pi/2) * 150
			setPropertyFromGroup("strumLineNotes", i, "x", nowX)
			setPropertyFromGroup("strumLineNotes", i, "y", nowY)
			setPropertyFromGroup("strumLineNotes", i, "direction", time * 45 + (downscroll and -90 or 90))
		else
			nowX = screenWidth / 2 - offset + math.sin(timeDeg / 4.1 + i) * 300
			nowY = screenHeight / 2 - offset + math.cos(timeDeg + i) * 150
			setPropertyFromGroup("strumLineNotes", i, "x", nowX)
			setPropertyFromGroup("strumLineNotes", i, "y", nowY)
			setPropertyFromGroup("strumLineNotes", i, "direction", -time * 45 + (downscroll and -90 or 90))
		end
		-- local angle = math.atan2(nowY - screenHeight / 2, nowX - screenWidth / 2)
	end
end

function noteTween4(time)
    local rsp0 = time * pi
	local rsp1 = rsp0 / 4 + pi / 2
	local rsp2 = rsp0 / 8
	local rsp3 = rsp0 / 4
	
	for i = 0,7 do
		if i < 4 then
			setPropertyFromGroup("strumLineNotes", i, "x", screenWidth / 2 - offset + math.cos(rsp0 + pi * i * 0.5) * 250 - math.tan(rsp1) * 40)
			setPropertyFromGroup("strumLineNotes", i, "y", screenHeight / 2 - offset + math.sin(rsp0 + pi * i * 0.5) * 250 - math.sin(rsp1) * 40)
			setPropertyFromGroup("strumLineNotes", i, "direction", time * 180 + 90 * i)
		else
			setPropertyFromGroup("strumLineNotes", i, "x", screenWidth / 2 - offset + math.cos(-rsp0 + pi * i * 0.5) * 100 + math.tan(rsp1) * 40)
			setPropertyFromGroup("strumLineNotes", i, "y", screenHeight / 2 - offset + math.sin(-rsp0 + pi * i * 0.5) * 100 + math.sin(rsp0) * 40)
			setPropertyFromGroup("strumLineNotes", i, "direction", -time * 180 + 90 * i)
		end
	end
end