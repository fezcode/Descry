-- rainbow.lua — paint every row a different color of the spectrum.
--
-- A pure plugin: it reads the document (descry.buffer.text), computes a hue
-- per line, and pushes one fg decoration per line via descry.decorations.
-- The engine has no idea what "rainbow" is — this is just the general
-- decoration API in use. Toggle with "rainbow rows" (Ctrl+Shift+P); it
-- recolors live as you type via the text_change event.

-- HSV->RGB for a row index; hue steps so adjacent rows differ.
local function hue(row)
    local h = (row * 32) % 360
    local s, v = 0.72, 1.0
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

local enabled = false

-- One fg decoration per source line. Offsets are 0-based buffer bytes, which
-- is exactly what descry.decorations.add expects.
local function apply()
    descry.decorations.clear()
    local t = descry.buffer.text()
    local n = #t
    local line_start, row = 0, 0
    for k = 1, n do
        if t:byte(k) == 10 then          -- '\n' at 0-based (k-1)
            if (k - 1) > line_start then
                local r, g, b = hue(row)
                descry.decorations.add(line_start, k - 1, { fg = { r, g, b } })
            end
            line_start = k               -- next line starts after the newline
            row = row + 1
        end
    end
    if n > line_start then               -- final line (no trailing newline)
        local r, g, b = hue(row)
        descry.decorations.add(line_start, n, { fg = { r, g, b } })
    end
end

local function refresh()
    if enabled then apply() else descry.decorations.clear() end
end

descry.register_action("rainbow_rows", function()
    enabled = not enabled
    refresh()
    descry.notify(enabled and "rainbow: on" or "rainbow: off")
end)

-- Keep the colors in sync with edits / file switches while enabled.
descry.on("text_change", function() if enabled then apply() end end)
descry.on("open",        function() if enabled then apply() end end)

descry.notify("[rainbow] loaded -- run 'rainbow rows' from the palette")
