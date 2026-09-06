function onUpdate(elapsed)    

    songPos = getSongPosition()
	local currentBeat = (songPos/5000)*(curBpm/60)
    
    noteTweenX(defaultPlayerStrumX0, 4, defaultPlayerStrumX0 - 100 *math.cos((currentBeat*0.25)*math.pi), 0.5)
    noteTweenX(defaultPlayerStrumX1, 5, defaultPlayerStrumX1 - 100 *math.cos((currentBeat*0.25)*math.pi), 0.5)
    noteTweenX(defaultPlayerStrumX2, 6, defaultPlayerStrumX2 - 100 *math.cos((currentBeat*0.25)*math.pi), 0.5)
    noteTweenX(defaultPlayerStrumX3, 7, defaultPlayerStrumX3 - 100 *math.cos((currentBeat*0.25)*math.pi), 0.5)
    
    noteTweenX(defaultOpponentStrumX0, 0, defaultOpponentStrumX0 + 100 *math.cos((currentBeat*0.25)*math.pi),  0.5)
    noteTweenX(defaultOpponentStrumX1, 1, defaultOpponentStrumX1 + 100 *math.cos((currentBeat*0.25)*math.pi),  0.5)
    noteTweenX(defaultOpponentStrumX2, 2, defaultOpponentStrumX2 + 100 *math.cos((currentBeat*0.25)*math.pi),  0.5)
    noteTweenX(defaultOpponentStrumX3, 3, defaultOpponentStrumX3 + 100 *math.cos((currentBeat*0.25)*math.pi),  0.5)

    doTweenAngle("cameraA", "camHUD", 3 * math.sin((currentBeat+0 *0.25) * math.pi), 0.01)

end

