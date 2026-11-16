---
title: Log
nav_order: 2
parent: 模组环境
grand_parent: Lua API
---

# Log

```lua
Log.debug("...")   -- 仅开发版写入文件
Log.info("...")
Log.warn("...")
Log.error("...")
```

参数可以是任意值，会用制表符连接。日志写到 `kmod/logs/kmod.log`，行首标签是模组 id，格式见[调试与日志]({% link guides/debugging.md %})。`print(...)` 等价于 `Log.info(...)`。
