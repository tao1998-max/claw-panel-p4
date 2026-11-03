-- Self-contained Lottie sample: a blue circle that scales up and down,
-- 60 frames at 30 fps over 200x200 pixels.
local LOTTIE_JSON = [[{"v":"5.7.4","fr":30,"ip":0,"op":60,"w":200,"h":200,"nm":"poc","ddd":0,"assets":[],"layers":[{"ddd":0,"ind":1,"ty":4,"nm":"circle","sr":1,"ks":{"o":{"a":0,"k":100},"r":{"a":0,"k":0},"p":{"a":0,"k":[100,100,0]},"a":{"a":0,"k":[0,0,0]},"s":{"a":1,"k":[{"t":0,"s":[60,60,100],"e":[100,100,100],"i":{"x":[0.5],"y":[1]},"o":{"x":[0.5],"y":[0]}},{"t":30,"s":[100,100,100],"e":[60,60,100],"i":{"x":[0.5],"y":[1]},"o":{"x":[0.5],"y":[0]}},{"t":60,"s":[60,60,100]}]}},"ao":0,"shapes":[{"ty":"gr","it":[{"ty":"el","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[80,80]},"nm":"Ellipse Path 1","mn":"ADBE Vector Shape - Ellipse"},{"ty":"fl","c":{"a":0,"k":[0.2,0.6,1,1]},"o":{"a":0,"k":100},"r":1,"bm":0,"nm":"Fill 1","mn":"ADBE Vector Graphic - Fill"}],"nm":"Group 1","np":2,"cix":2,"bm":0,"ix":1,"mn":"ADBE Vector Group"}],"ip":0,"op":60,"st":0,"bm":0}]}]]

local delay = require("delay")
local lvgl = require("lvgl")

lvgl.init({
    buffer_lines = 10,
    tick_ms = 5,
    task_period_ms = 10,
})

local ok, err = pcall(function()
    local scr = lvgl.create_screen()

    local anim = lvgl.lottie(scr, {
        w = 200,
        h = 200,
        src_data = LOTTIE_JSON,
        align = "center",
    })
    assert(anim:is_loaded())
    assert(anim:is_playing())
    assert(anim:get_total_frames() == 60)
    assert(anim:get_duration() == 1000)

    scr:load()
    delay.delay_ms(300)
    assert(anim:get_time() > 0)

    assert(anim:pause() == true)
    assert(anim:is_playing() == false)
    local paused_at = anim:get_time()
    delay.delay_ms(150)
    assert(anim:get_time() == paused_at)

    assert(anim:resume() == true)
    assert(anim:is_playing() == true)

    assert(anim:restart() == true)
    assert(anim:get_time() < 100)

    local anim2 = lvgl.lottie(scr, {
        w = 96,
        h = 96,
        src_data = LOTTIE_JSON,
        autoplay = false,
    })
    assert(anim2:is_loaded())
    assert(anim2:is_playing() == false)
    anim2:resume()
    assert(anim2:is_playing() == true)
    assert(anim2:set_src_data(LOTTIE_JSON) == true)

    delay.delay_ms(2000)
end)

lvgl.deinit()

if not ok then
    error(err)
end
