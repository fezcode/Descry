-- Sample Descry plugin.
-- Plugins live under `data/plugins/` (configurable via plugin_path in init.lua)
-- and are loaded once on startup. Use the command palette (Ctrl+Shift+P)
-- to run any action this file registers, or the Plugins overlay
-- (Ctrl+Alt+P) to see what loaded.

-- Pop up a modal greeting dialog.
descry.register_action("say_hello", function()
    descry.dialog("Hello", "Hello from the plugin system!")
end)

-- A second action so the Plugins overlay has multiple rows to show off.
descry.register_action("hello_count", function()
    descry.notify("plugin actions registered by hello.lua: 2")
end)

-- Notify the user that this plugin loaded (visible on stderr + status bar).
descry.notify("[hello plugin] loaded — try 'Say Hello' from Ctrl+Shift+P")
