-- word_count.lua — a real word/character counter built on the document API.
--
-- Before the buffer bridge existed this plugin could only fake a 0. Now it
-- reads the live document with descry.buffer.text(), so the counts are real,
-- and it updates a status-bar tally on every edit via descry.on("text_change").

local function counts()
    local t = descry.buffer.text()
    local _, words = t:gsub("%S+", "")   -- runs of non-space = words
    return words, #t
end

-- Ctrl+Shift+P → "word count": pop a dialog with the totals.
descry.register_action("word_count", function()
    local words, chars = counts()
    descry.dialog("Word count",
        string.format("%d words\n%d characters", words, chars))
end)

-- Live tally in the status bar as you type.
descry.on("text_change", function()
    descry.notify("words: " .. (select(1, counts())))
end)

descry.notify("[word_count] loaded")
