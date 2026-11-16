---
title: Mod
nav_order: 1
parent: 模组环境
grand_parent: Lua API
---

# Mod

只读的模组信息。

| 字段 | 说明 |
|---|---|
| `Mod.id` | 模组 id |
| `Mod.name` | `mod.json` 里的 `name`（默认文字，不是翻译后的） |
| `Mod.version` | 版本号字符串 |
| `Mod.dir` | 模组目录的完整路径 |
| `Mod.path(rel)` | 把包内相对路径拼成完整路径，如 `Mod.path("assets/x.dat")` |

```lua
Log.info(Mod.id .. " " .. Mod.version .. " at " .. Mod.dir)
local f = io.open(Mod.path("data.txt"), "rb")
```
