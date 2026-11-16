-- Optional: a native mod can also have a Lua part. The functions the C++ side registers are in the table `Native`.
print("hello from Lua; Native.add(2, 3) = " .. tostring(Native.add(2, 3)))
