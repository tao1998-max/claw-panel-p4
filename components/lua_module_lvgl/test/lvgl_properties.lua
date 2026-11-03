local delay = require("delay")
local lvgl = require("lvgl")

lvgl.init({
    buffer_lines = 10,
    tick_ms = 5,
    task_period_ms = 10,
})

local ok, err = pcall(function()
    local scr = lvgl.create_screen()

    -- Widget property: text getter/setter (label)
    local label = lvgl.label(scr, { text = "before", x = 12, y = 24 })
    assert(label:get_property("text") == "before")
    assert(label:set_property("text", "after") == true)
    assert(label:get_property("text") == "after")

    -- The motivating case: read back what a textarea contains
    local ta = lvgl.textarea(scr, { placeholder = "type", one_line = true })
    assert(ta:set_property("text", "hello") == true)
    assert(ta:get_property("text") == "hello")

    -- Style property by name
    assert(label:set_property("text_color", "#ff0000") == true)
    assert(label:get_property("text_color") == 0xff0000)

    -- Flag property (INT typed, non-zero means set)
    assert(label:set_property("flag_hidden", 1) == true)
    assert(label:get_property("flag_hidden") ~= 0)
    assert(label:set_property("flag_hidden", 0) == true)
    assert(label:get_property("flag_hidden") == 0)

    -- Paired property: slider value, second value (anim) defaults to 0/false
    local slider = lvgl.slider(scr, { min = 0, max = 100, value = 10 })
    assert(slider:set_property("value", 55) == true)
    assert(slider:get_property("value") == 55)

    -- Unknown names must fail loudly
    local unknown_ok = pcall(function()
        label:set_property("no_such_property", 1)
    end)
    assert(unknown_ok == false)

    -- Geometry getters read laid-out coordinates, so make the screen active
    -- and give the LVGL task time to run layout before checking position.
    scr:load()
    delay.delay_ms(100)
    assert(label:get_property("x") == 12)
    assert(label:set_property("x", 30) == true)
    delay.delay_ms(100)
    assert(label:get_property("x") == 30)

    delay.delay_ms(2000)
end)

lvgl.deinit()

if not ok then
    error(err)
end
