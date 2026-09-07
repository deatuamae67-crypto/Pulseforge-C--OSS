function opponentNoteHit()
       health = getProperty('health')
    if getProperty('health') > 1 then
       setProperty('health', health- 0.0050);
	end
end

function onUpdate(elapsed)
  if curStep >= 0 then
    songPos = getSongPosition()
    local currentBeat = (songPos/1000)*(bpm/120)
    doTweenY(dadTweenY, 'dad', 290-130*math.sin((currentBeat*0.25)*math.pi),0.001)
  end
endfunction opponentNoteHit(id, direction, noteType, isSustainNote)
	cameraShake(game, 0.015, 0.2)
	cameraSetTarget('dad')
	characterPlayAnim('gf', 'scared', true)
	doTweenZoom('camerazoom','camGame',1.1,0.2,'quadInOut')
end