---
title: NotifyOnNewObject
nav_order: 12
parent: 全局函数
grand_parent: Lua API
---

# NotifyOnNewObject

```lua
NotifyOnNewObject(className, fn)
```

新创建了某个类（含子类）的对象时调用 `fn`。

## 参数

| # | 类型 | 说明 |
|---|---|---|
| 1 | string | 类名，规则同 [`FindFirstOf`]({% link lua-api/global-functions/findfirstof.md %}) |
| 2 | function | `fn(obj)`，`obj` 是新对象的 [`UObject`]({% link lua-api/classes/uobject.md %}) 包装 |

## 说明

- 实现方式是每次 tick 检查全局对象表里新增的槽位，所以在两次 tick 之间创建又销毁的对象会漏掉。
- 对象刚创建时可能还没初始化完。需要读写属性的话，用 [`ExecuteWithDelay`]({% link lua-api/global-functions/executewithdelay.md %}) 稍后再处理。
- 默认对象（CDO）不触发。
- 模组被禁用或重新加载时自动取消。

## 示例

```lua
NotifyOnNewObject("PlayerController", function(pc)
  ExecuteWithDelay(500, function()
    if pc:IsValid() then Log.info("new controller: " .. pc:GetPathName()) end
  end)
end)
```
