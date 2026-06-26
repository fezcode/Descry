-- rainbow.lua — paint every rendered row a different color of the spectrum.
--
-- Coloring is a render-side effect, so this relies on the descry.rainbow()
-- bridge hook (the buffer/vault API alone can't change colors). Toggling it
-- recolors both the preview and the editor: each row's hue steps around the
-- spectrum, so the document reads as a rainbow gradient top to bottom.
--
-- Run "rainbow rows" from the command palette (Ctrl+Shift+P), or bind it to a
-- key in Settings > Keybindings (F1).

local on = false

descry.register_action("rainbow_rows", function()
    on = not on
    descry.rainbow(on)                       -- explicit on/off
    descry.notify(on and "rainbow: on" or "rainbow: off")
end)

descry.notify("[rainbow] loaded -- run 'rainbow rows' from the palette")
