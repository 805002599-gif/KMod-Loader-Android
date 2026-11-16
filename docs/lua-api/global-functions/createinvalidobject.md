---
title: CreateInvalidObject
nav_order: 5
parent: 全局函数
grand_parent: Lua API
---

# CreateInvalidObject

```lua
local obj = CreateInvalidObject()
```

返回一个无效对象（`IsValid()` 为 `false`）。用来给变量做初始值，免得在判断前后类型不一致。
