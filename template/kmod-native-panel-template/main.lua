-- Native.open_panel() opens the settings panel; Native.volume() returns the saved value.
print("saved volume: " .. Native.volume())
ExecuteWithDelay(2000, function() Native.open_panel() end)
