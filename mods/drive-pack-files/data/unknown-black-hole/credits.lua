IntroSubText2Size = 20
IntroTagWidth = 15	--Width of the box's tag thingy.
function onCreate()
--text for the artist name
    makeLuaText('JukeBoxSubText2', 'GFstyle', 300, -305-IntroTagWidth, 260)
    setTextAlignment('JukeBoxSubText2', 'left')
    setObjectCamera('JukeBoxSubText2', 'other')
    setTextSize('JukeBoxSubText2', IntroSubText2Size)
    addLuaText('JukeBoxSubText2')
end
function onSongStart()
    doTweenX('MoveInFive', 'JukeBoxSubText2', 0, 1, 'CircInOut')
    runTimer('JukeBoxWait', 3, 1)
end
function onTimerCompleted(tag, loops, loopsLeft)
    if tag == 'JukeBoxWait' then
        doTweenX('MoveOutFive', 'JukeBoxSubText2', -450, 1.5, 'CircInOut')
    end
end