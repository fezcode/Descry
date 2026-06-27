-- format.lua — reflow prose paragraphs to a maximum line width WITHOUT
-- touching code blocks, tables, headings, lists, blockquotes, display math,
-- or YAML frontmatter. Run "format document" from the command palette.
--
-- Config: "format.max_line" (default 80). Edit it in the Plugins overlay
-- (Ctrl+Alt+P → Config), or in settings.lua under the `plugins` table.

descry.config("format.max_line", "80")   -- register so the overlay lists it

local function is_blank(s)  return s:match("^%s*$") ~= nil end
local function is_fence(s)  return s:match("^%s*```") or s:match("^%s*~~~") end
local function is_dollar_fence(s) return s:match("^%s*%$%$%s*$") ~= nil end

-- Lines that must never be reflowed (each stays verbatim on its own line).
local function is_structural(s)
    return s:match("^%s*#")            -- heading
        or s:match("^%s*[%-%*%+]%s")   -- bullet list item
        or s:match("^%s*%d+[%.%)]%s")  -- ordered list item
        or s:match("^%s*>")            -- blockquote
        or s:match("|")                -- table row
        or s:match("^%s*%$%$")         -- a display-math line
        or s:match("^    ")            -- indented code (4 spaces)
        or s:match("^\t")              -- indented code (tab)
        or s:match("^%s*<")            -- raw HTML-ish
end

-- Greedy word wrap of a list of words to `maxw` columns.
local function wrap(words, maxw)
    local out, line = {}, ""
    for _, w in ipairs(words) do
        if line == "" then
            line = w
        elseif #line + 1 + #w <= maxw then
            line = line .. " " .. w
        else
            out[#out + 1] = line
            line = w
        end
    end
    if line ~= "" then out[#out + 1] = line end
    return out
end

local function format_doc()
    local maxw = tonumber(descry.config("format.max_line", "80")) or 80
    if maxw < 20 then maxw = 20 end

    local text    = descry.buffer.text()
    local ends_nl = text:sub(-1) == "\n"
    local body    = ends_nl and text or (text .. "\n")

    local lines = {}
    for line in body:gmatch("([^\n]*)\n") do lines[#lines + 1] = line end

    local out, para = {}, {}
    local function flush()
        if #para == 0 then return end
        for _, l in ipairs(wrap(para, maxw)) do out[#out + 1] = l end
        para = {}
    end

    local in_fence, in_math, in_fm = false, false, false
    for i, line in ipairs(lines) do
        if i == 1 and line:match("^%-%-%-%s*$") then
            in_fm = true; out[#out + 1] = line
        elseif in_fm then
            out[#out + 1] = line
            if line:match("^%-%-%-%s*$") then in_fm = false end
        elseif is_fence(line) then
            flush(); in_fence = not in_fence; out[#out + 1] = line
        elseif in_fence then
            out[#out + 1] = line
        elseif is_dollar_fence(line) then
            flush(); in_math = not in_math; out[#out + 1] = line
        elseif in_math then
            out[#out + 1] = line
        elseif is_blank(line) or is_structural(line) then
            flush(); out[#out + 1] = line
        else
            for w in line:gmatch("%S+") do para[#para + 1] = w end
        end
    end
    flush()

    local result = table.concat(out, "\n")
    if ends_nl then result = result .. "\n" end
    descry.buffer.set_text(result)
end

descry.register_action("format_document", function()
    format_doc()
    local maxw = tonumber(descry.config("format.max_line", "80")) or 80
    descry.notify("formatted to " .. maxw .. " columns")
end)

descry.notify("[format] loaded -- run 'format document' from the palette")
