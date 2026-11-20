--[[
@title Quick ISO Change
@chdk_version 1.6
#hold_ms=450 "FUNC/SET hold (ms)" [200 1200]
]]

-- On the A620, run this script in ALT to open the picker once.
-- On the A620, EXCCD+ also offers a direct Quick ISO shortcut in Script settings.
-- On other cameras the script stays armed for the FUNC/SET hold.
-- Use LEFT/RIGHT, then SET or half-shutter to apply. MENU cancels.
--
-- ISO mode IDs and Canon ISO values from platform/*/shooting.c in the CHDK mirror:
-- https://github.com/01luna/CHDK/tree/92208bf004e193c30b625065889bd533732a524b/platform
-- The [0] and [-1] entries are Auto and High ISO Auto.

local iso_profiles = {
    {
        [[a1000 a2000 a2100 a470 a550 a560 a570 a580 a590 a650 a720 d10 g7 g9 ixus100_sd780
        ixus110_sd960 ixus130_sd1400 ixus70_sd1000 ixus75_sd750 ixus80_sd1100
        ixus850_sd800 ixus860_sd870 ixus900_sd900 ixus950_sd850 ixus95_sd1200
        ixus960_sd950 ixus980_sd990 sx100is tx1]],
        { [-1] = 1, [0] = 0, 80, 100, 200, 400, 800, 1600 },
    },
    {
        [[a1100 g11 g12 ixus265_elph340hs ixus275_elph350hs sx400is sx510hs sx600hs sx610hs
        sx620hs sx710hs sx730hs]],
        { [0] = 0, 80, 100, 200, 400, 800, 1600, 3200 },
    },
    {
        [[a1200 a2200 a3200 a3300 a480 a490 a495 ixus132_elph115 ixus135_elph120 ixus140_elph130
        ixus990_sd970 sx130is sx150is sx30]],
        { [0] = 0, 80, 100, 200, 400, 800, 1600 },
    },
    {
        [[a1300 a1400 a2300 a2400 a2500 a2600 a3000 a3100 a3400 a4000 a800 a810 ixus145_elph135
        ixus150_elph140 ixus155_elph150 ixus160_elph160 ixus170_elph170
        ixus175_elph180 ixus185_elph185 ixus190_elph200 sx170is sx410is sx420is
        sx430is]],
        { [0] = 0, 100, 200, 400, 800, 1600 },
    },
    {
        [[a410]],
        { [0] = 0, 50, 100, 200 },
    },
    {
        [[a420 a430]],
        { [0] = 0, 64, 100, 200, 400 },
    },
    {
        [[a450 a460]],
        { [0] = 0, 80, 100, 200, 400 },
    },
    {
        [[a530 a540]],
        { [0] = 0, 80, 100, 200, 400, 800 },
    },
    {
        [[a610 a620 ixus30_sd200 ixus40_sd300 ixus50_sd400 ixus55_sd450 ixus700_sd500
        ixus750_sd550 ixusizoom_sd30 ixusw_sd430 s2is s80]],
        { [0] = 0, 50, 100, 200, 400 },
    },
    {
        [[a630 a640 a700 a710 ixus800_sd700 s3is]],
        { [-1] = 1, [0] = 0, 80, 100, 200, 400, 800 },
    },
    {
        [[d20 d30 ixus115_elph100hs ixus125_elph110hs ixus180_elph190 ixus220_elph300hs
        ixus230_elph310hs ixus240_elph320hs ixus285_elph360hs ixus310_elph500hs n
        n_facebook sx160is sx220hs sx230hs sx240hs sx260hs sx280hs sx40hs sx500is
        sx520hs sx530hs sx60hs sx700hs]],
        { [0] = 0, 100, 200, 400, 800, 1600, 3200 },
    },
    {
        [[g10 ixus120_sd940 ixus200_sd980 ixus85_sd770 ixus870_sd880 ixus90_sd790 ixus970_sd890
        s5is sx1 sx10 sx110is sx120is sx20 sx200is sx210is]],
        { [-1] = 1, [0] = 0, 80, 100, 200, 400, 800, 1600, 3200 },
    },
    {
        [[g15 s110]],
        { [0] = 0, 80, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600,
          2000, 2500, 3200, 4000, 5000, 6400, 8000, 10000, 12800 },
    },
    {
        [[g16]],
        { [0] = 0, 80, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600,
          2000, 2500, [18] = 3200, [19] = 4000, [20] = 5000, [21] = 6400, [22] = 8000, [23] =
          10000, [24] = 12800 },
    },
    {
        [[g1x m10 m3]],
        { [0] = 0, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600, 2000,
          2500, 3200, 4000, 5000, 6400, 8000, 10000, 12800 },
    },
    {
        [[g5x g7x g7x2]],
        { [0] = 0, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600, 2000, 2500,
          3200, 4000, 5000, 6400, 8000, 10000, 12800 },
    },
    {
        [[ixus1000_sd4500]],
        { [-1] = 1, [0] = 0, 125, 200, 400, 800, 1600, 3200 },
    },
    {
        [[ixus105_sd1300]],
        { [0] = 0, 80, 100, [5] = 200, [8] = 400, [11] = 800, [14] = 1600 },
    },
    {
        [[ixus255_elph330hs]],
        { [0] = 0, 80, 100, 200, 400, 800, 1600, 3200, 6400 },
    },
    {
        [[ixus300_sd4000]],
        { [0] = 0, 125, 200, 400, 800, 1600, 3200 },
    },
    {
        [[ixus60_sd600 ixus65_sd630]],
        { [0] = 0, 50, 100, 200, 400, 800 },
    },
    {
        [[s100 sx50hs]],
        { [0] = 0, 80, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600,
          2000, 2500, 3200, 4000, 5000, 6400 },
    },
    {
        [[s90 s95]],
        { [0] = 0, 80, 100, 125, 160, 200, 250, 320, 400, 500, 640, 800, 1000, 1250, 1600,
          2000, 2500, 3200 },
    },
}

local build = get_buildinfo()
local model = build.platform
local is_a620 = model == "a620"
local is_a620_100f = is_a620 and build.platsub == "100f"
local hold_time = hold_ms or 450

local function get_camera_iso_choices()
    for _, profile in ipairs(iso_profiles) do
        for name in string.gmatch(profile[1], "%S+") do
            if name == model then
                local choices = {}
                for id, value in pairs(profile[2]) do
                    local label = tostring(value)
                    if id == 0 then label = "AUTO" end
                    if id == -1 then label = "HI" end
                    choices[#choices + 1] = { id = id, value = value, label = label }
                end
                table.sort(choices, function(a, b) return a.value < b.value end)
                return choices
            end
        end
    end
end

local all_choices = get_camera_iso_choices()
if not all_choices then
    print("No ISO profile for " .. model)
    return
end

local capmode = require("capmode")
local still_modes = { P = true, TV = true, AV = true, M = true }

local function canon_ui_idle()
    if not is_a620_100f then
        return true
    end
    -- Read-only checks from the A620 100f CHDK OSD port.
    -- Keep FUNC/SET native in the Canon menu, FUNC menu and image review.
    return peek(0x8DC4, 4) == 0x8DC0
       and peek(0x8221, 1) == 0
       and peek(0x7D574, 4) == 0
end

local TITLE = "Quick ISO Change"
local FONT_WIDTH = 8
local BLACK, WHITE = 257, 258    -- CHDK standard script colors
local PANEL_HEIGHT = 51

local function floor_div(numerator, denominator)
    -- CHDK Lua uses integer numbers and does not provide math.floor.
    return (numerator - numerator % denominator) / denominator
end

local function get_choices(mode)
    if is_a620 and mode == "M" then
        -- Canon's A620 Advanced Guide, p. 136: M mode has no Auto ISO.
        local choices = {}
        for _, option in ipairs(all_choices) do
            if option.id ~= 0 then
                choices[#choices + 1] = option
            end
        end
        return choices
    end
    return all_choices
end

local function draw_picker(choices, selected)
    local screen_width = get_gui_screen_width()
    local cell_width = 42
    for _, option in ipairs(choices) do
        cell_width = math.max(cell_width, #option.label * FONT_WIDTH + 10)
    end

    local count = math.min(#choices, math.max(1, floor_div(screen_width - 32, cell_width)))
    local first = math.max(1, math.min(selected - floor_div(count, 2), #choices - count + 1))
    local width = math.max(#TITLE * FONT_WIDTH + 20, count * cell_width + 28)
    local x = floor_div(screen_width - width, 2)
    local y = floor_div(get_gui_screen_height() - PANEL_HEIGHT, 2)
    local cells_x = x + floor_div(width - count * cell_width, 2)

    draw_rect_filled(x, y, x + width - 1, y + PANEL_HEIGHT - 1, WHITE, BLACK)
    draw_string(x + floor_div(width - #TITLE * FONT_WIDTH, 2), y + 4, TITLE, WHITE, BLACK)

    for column = 0, count - 1 do
        local index = first + column
        local label = choices[index].label
        local left = cells_x + column * cell_width
        local label_x = left + floor_div(cell_width - #label * FONT_WIDTH, 2)
        if index == selected then
            draw_rect_filled(left + 1, y + 24, left + cell_width - 2, y + 45, WHITE, WHITE)
            draw_string(label_x, y + 27, label, BLACK, WHITE)
        else
            draw_string(label_x, y + 27, label, WHITE, BLACK)
        end
    end

    if first > 1 then draw_string(x + 3, y + 27, "<", WHITE, BLACK) end
    if first + count <= #choices then
        draw_string(x + width - 11, y + 27, ">", WHITE, BLACK)
    end
end

local owns_alt = false

local function leave_picker()
    owns_alt = false
    exit_alt()
    set_draw_title_line(1)
end

-- Called if MENU interrupts the script while the overlay owns the keyboard.
function restore()
    set_draw_title_line(1)
    if owns_alt then
        exit_alt()
    end
end

local function use_picker(mode, opening_set_down)
    local choices = get_choices(mode)
    local current_id = get_iso_mode()
    local selected = 1
    for index, option in ipairs(choices) do
        if option.id == current_id then selected = index end
    end

    local set_was_down = opening_set_down
    local left_was_down = is_pressed("left")
    local right_was_down = is_pressed("right")
    local redraw = true
    local last_draw = get_tick_count()
    while capmode.get_name() == mode and get_alt_mode() and canon_ui_idle() do
        local set_down = is_pressed("set")
        local half_down = is_pressed("shoot_half")
        local left_down = is_pressed("left")
        local right_down = is_pressed("right")

        if half_down or (set_down and not set_was_down) then
            local selected_id = choices[selected].id
            if selected_id ~= current_id then set_iso_mode(selected_id) end
            -- Consume the SET release and any held arrows before returning the
            -- keys to Canon. A held half-shutter can pass through on that scan.
            break
        end

        local next_index = selected
        if left_down and not left_was_down then next_index = math.max(1, selected - 1) end
        if right_down and not right_was_down then next_index = math.min(#choices, selected + 1) end
        if next_index ~= selected then
            selected = next_index
            redraw = true
        end

        local now = get_tick_count()
        if redraw or now - last_draw >= 150 then
            draw_picker(choices, selected)
            last_draw = now
            redraw = false
        end

        set_was_down, left_was_down, right_was_down = set_down, left_down, right_down
        sleep(0)
    end

    draw_clear()
    while get_alt_mode()
          and (is_pressed("set") or is_pressed("left") or is_pressed("right")) do
        sleep(0)
    end
end

local function handle_set(mode)
    -- On this keyboard loop the script runs before the physical key state is
    -- passed to Canon. Take ownership on the first scan of the SET press.
    owns_alt = true
    enter_alt()
    set_draw_title_line(0)

    local started = get_tick_count()
    while is_pressed("set") and get_tick_count() - started < hold_time do
        if is_pressed("shoot_half") or capmode.get_name() ~= mode or not canon_ui_idle() then
            -- A shutter press or mode change takes priority during the hold.
            while is_pressed("set") and get_alt_mode() do sleep(0) end
            leave_picker()
            return
        end
        sleep(0)
    end

    local still_down = is_pressed("set")
    if get_tick_count() - started >= hold_time then
        use_picker(mode, still_down)
    elseif get_alt_mode() then
        -- A tap is a regular Canon FUNC/SET click. The simulated press goes
        -- through CHDK's key timing, without sending a second physical press.
        click("set")
    end

    leave_picker()
end

if is_a620_100f then
    -- The firmware handles the mapped key. Running the script manually
    -- opens the same choices without leaving a background key watcher.
    if autostarted() or get_autostart() == 3 then return end
    local mode = capmode.get_name()
    if not still_modes[mode] or not canon_ui_idle() then
        print("Use Quick ISO in P, Tv, Av or M.")
        return
    end
    set_exit_key("menu")
    set_console_autoredraw(0)
    cls()
    set_draw_title_line(0)
    repeat sleep(10) until not is_pressed("shoot_half") or not get_alt_mode()
    use_picker(mode, is_pressed("set"))
    set_draw_title_line(1)
    return
end

set_exit_key("menu")
set_console_autoredraw(0)
cls()
exit_alt()

local set_was_down = is_pressed("set")
while true do
    local set_down = is_pressed("set")
    if set_down and not set_was_down and not get_alt_mode() and not is_pressed("shoot_half") then
        local mode = capmode.get_name()
        if still_modes[mode] and canon_ui_idle() and not get_shooting() then
            handle_set(mode)
            set_down = is_pressed("set")
        end
    end
    set_was_down = set_down
    sleep(0)
end
