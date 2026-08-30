-- format.lua — reflow prose paragraphs to a maximum line width WITHOUT
-- touching code blocks, tables, headings, lists, blockquotes, display math,
-- or YAML frontmatter, and collapse runs of blank lines. Run "format
-- document" from the command palette.
--
-- Settings (Plugins overlay, Ctrl+Alt+P → format → settings, or the
-- `plugins` table in settings.lua):
--   format.max_column  wrap prose at this column           (default 80)
--   format.max_line    max consecutive blank lines kept,
--                      0 = leave blank lines alone         (default 2)

local function cfg_max_column()
    return descry.config("format.max_column", 80,
        { type = "number", min = 20, max = 400,
          desc = "Wrap paragraphs at this column" })
end
local function cfg_max_line()
    return descry.config("format.max_line", 2,
        { type = "number", min = 0, max = 20,
          desc = "Max consecutive blank lines kept (0 = unlimited)" })
end
-- Declare both so the overlay lists them before the first run.
cfg_max_column(); cfg_max_line()

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
        or s:match("^%s*[%-%*_][%s%-%*_]*$") -- thematic break (---, ***)
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
    local maxw = math.floor(tonumber(cfg_max_column()) or 80)
    if maxw < 20 then maxw = 20 end
    local maxblank = math.floor(tonumber(cfg_max_line()) or 2)
    if maxblank < 0 then maxblank = 0 end

    local text    = descry.buffer.text()
    local ends_nl = text:sub(-1) == "\n"
    local body    = ends_nl and text or (text .. "\n")

    local lines = {}
    for line in body:gmatch("([^\n]*)\n") do lines[#lines + 1] = line end

    local out, para = {}, {}
    local blank_run = 0
    local function push(l)
        if is_blank(l) then
            blank_run = blank_run + 1
            if maxblank > 0 and blank_run > maxblank then return end
            out[#out + 1] = ""
        else
            blank_run = 0
            out[#out + 1] = l
        end
    end
    local function flush()
        if #para == 0 then return end
        for _, l in ipairs(wrap(para, maxw)) do push(l) end
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
            flush(); in_fence = not in_fence; push(line)
        elseif in_fence then
            out[#out + 1] = line; blank_run = 0   -- code is verbatim
        elseif is_dollar_fence(line) then
            flush(); in_math = not in_math; push(line)
        elseif in_math then
            out[#out + 1] = line; blank_run = 0
        elseif is_blank(line) or is_structural(line) then
            flush(); push(line)
        else
            for w in line:gmatch("%S+") do para[#para + 1] = w end
        end
    end
    flush()

    local result = table.concat(out, "\n")
    if ends_nl then result = result .. "\n" end
    descry.buffer.set_text(result)
    return #out, maxw
end

descry.register_action("format_document", function()
    local lines, maxw = format_doc()
    -- The wrap is a SOURCE change; markdown preview reflows it back into one
    -- paragraph, so switch to edit mode where the wrapping is actually visible.
    descry.set_edit_mode(true)
    descry.notify("formatted to " .. maxw .. " cols (" .. lines .. " lines)")
end)

descry.notify("[format] loaded -- run 'format document' from the palette")
