-- Create a minimal launcher App package in the DATA App root.
-- Called by the app_creator skill with args:
--   app_id       required, 1-63 chars of [A-Za-z0-9_-]
--   display_name optional launcher name (Chinese is fine; the launcher has a CJK font)
--   title        optional on-screen title, ASCII only (the App has no TTF)
--   visible      optional boolean, default true
local storage = require("storage")

local a = args or {}
local app_id = a.app_id
local display_name = a.display_name or app_id
local title = a.title or display_name
local visible = a.visible

if type(app_id) ~= "string" or not app_id:match("^[%w_%-]+$") or
        #app_id < 1 or #app_id > 63 then
    error("app_id must be 1-63 chars of [A-Za-z0-9_-]")
end
if type(display_name) ~= "string" or #display_name == 0 then
    display_name = app_id
end
if type(title) ~= "string" or #title == 0 then
    title = display_name
end
if title:find("[\128-\255]") then
    error("title must be ASCII: this App has no bundled TTF font; " ..
          "use English on screen, or copy a TTF into assets and load it")
end

local root = storage.join_path(storage.get_root_dir(), "apps")
local dir = storage.join_path(root, app_id)
local scripts = storage.join_path(dir, "scripts")
storage.mkdir(root)
storage.mkdir(dir)
storage.mkdir(scripts)

local main_lua = string.format([[
local lvgl = require("lvgl")
local system = require("system")
local delay = require("delay")

lvgl.init({ buffer_lines = 40, tick_ms = 5, task_period_ms = 10 })

local scr = lvgl.create_screen()
scr:set_style({ bg_color = "#0b1116", pad = 0 })
scr:clear_flag("SCROLLABLE")

lvgl.label(scr, {
  text = %q, align = "center", y = -48,
  font = "montserrat_20", text_color = "#e6f2f7",
})
local clock = lvgl.label(scr, {
  text = "--:--:--", align = "center", y = 8,
  font = "montserrat_32", text_color = "#62d4ff",
})
lvgl.label(scr, {
  text = "edit scripts/main.lua", align = "center", y = 64,
  text_color = "#5b7885",
})
scr:load()

local last = ""
while not claw.abort_requested() do
  local stamp = os.date("%%H:%%M:%%S")
  if stamp ~= last then
    last = stamp
    clock:set_text(stamp)
  end
  lvgl.process_events(20)
  delay.delay_ms(100)
end
lvgl.deinit()
]], title)

local manifest = string.format([[
{
  "schema_version": 1,
  "id": %q,
  "display_name": %q,
  "entry": "scripts/main.lua",
  "version": "1.0.0",
  "visible": %s
}
]], app_id, display_name, visible == false and "false" or "true")

storage.write_file(storage.join_path(scripts, "main.lua"), main_lua)
storage.write_file(storage.join_path(dir, "launcher.json"), manifest)

print("scaffolded " .. dir)
