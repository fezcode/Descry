-- rainbow.lua — paint every row a different color of the spectrum.
--
-- A pure plugin: it reads the document (descry.buffer.text), computes a hue
-- per line, and pushes one fg decoration per line via descry.decorations.
-- The engine has no idea what "rainbow" is — this is just the general
-- decoration API in use.
--
-- Turning it on / off (any of these work):
--   * "rainbow rows" in the command palette (Ctrl+Shift+P) toggles it;
--   * the `rainbow.enabled` switch in the Plugins overlay (Ctrl+Alt+P →
--     rainbow → settings);
--   * the plugin's on/off toggle in the same overlay unloads it entirely.
-- The choice is remembered in settings.lua (`plugins["rainbow.enabled"]`).

-- HSV->RGB for a row index; hue steps so adjacent rows differ.
local function hue(row, step, sat)
    local h = (row * step) % 360
    local s, v = sat, 1.0
    local c = v * s
    local x = c * (1 - math.abs((h / 60) % 2 - 1))
    local m = v - c
    local r, g, b = 0, 0, 0
    if     h <  60 then r, g, b = c, x, 0
    elseif h < 120 then r, g, b = x, c, 0
    elseif h < 180 then r, g, b = 0, c, x
    elseif h < 240 then r, g, b = 0, x, c
    elseif h < 300 then r, g, b = x, 0, c
    else                r, g, b = c, 0, x end
    return math.floor((r + m) * 255 + 0.5),
           math.floor((g + m) * 255 + 0.5),
           math.floor((b + m) * 255 + 0.5)
end

-- Declared settings (typed, so the Plugins overlay shows real editors).
local function cfg_enabled()
    return descry.config("rainbow.enabled", false,
        { type = "bool", desc = "Color every line of the note" })
end
local function cfg_step()
    return descry.config("rainbow.hue_step", 32,
        { type = "number", min = 1, max = 180,
          desc = "Hue difference between neighbouring lines (degrees)" })
end
local function cfg_sat()
    return descry.config("rainbow.saturation", 72,
        { type = "number", min = 0, max = 100,
          desc = "Color saturation, 0 = grey, 100 = vivid" })
end

-- One fg decoration per source line. Offsets are 0-based buffer bytes, which
-- is exactly what descry.decorations.add expects.
local function apply()
    descry.decorations.clear()
    local step, sat = cfg_step(), cfg_sat() / 100
    local t = descry.buffer.text()
    local n = #t
    local line_start, row = 0, 0
    for k = 1, n do
        if t:byte(k) == 10 then          -- '\n' at 0-based (k-1)
            if (k - 1) > line_start then
                local r, g, b = hue(row, step, sat)
                descry.decorations.add(line_start, k - 1, { fg = { r, g, b } })
            end
            line_start = k               -- next line starts after the newline
            row = row + 1
        end
    end
    if n > line_start then               -- final line (no trailing newline)
        local r, g, b = hue(row, step, sat)
        descry.decorations.add(line_start, n, { fg = { r, g, b } })
    end
end

local function refresh()
    if cfg_enabled() then apply() else descry.decorations.clear() end
end

descry.register_action("rainbow_rows", function()
    local on = not cfg_enabled()
    descry.config_set("rainbow.enabled", on)     -- persisted; fires config_change
    refresh()
    descry.notify(on and "rainbow: on" or "rainbow: off")
end)

-- Keep the colors in sync with edits / file switches / settings changes.
descry.on("text_change",   refresh)
descry.on("open",          refresh)
descry.on("config_change", refresh)

-- Declare the settings up front so they show in the overlay before the
-- first toggle, then paint the current note if the switch is already on.
cfg_step(); cfg_sat()
refresh()

descry.notify("[rainbow] loaded -- toggle with 'rainbow rows' or in Plugins")
