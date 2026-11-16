---
title: 全局函数
nav_order: 2
parent: Lua API
has_children: true
has_toc: false
---

# 全局函数

| 分组 | 函数 |
|---|---|
| 查找对象 | [`FindFirstOf`]({% link lua-api/global-functions/findfirstof.md %})、[`FindAllOf`]({% link lua-api/global-functions/findallof.md %})、[`FindObject`]({% link lua-api/global-functions/findobject.md %})、[`StaticFindObject`]({% link lua-api/global-functions/staticfindobject.md %})、[`CreateInvalidObject`]({% link lua-api/global-functions/createinvalidobject.md %}) |
| 加载资源 | [`LoadAsset`]({% link lua-api/global-functions/loadasset.md %}) |
| 钩子 | [`RegisterHook`]({% link lua-api/global-functions/registerhook.md %})、[`UnregisterHook`]({% link lua-api/global-functions/unregisterhook.md %}) |
| 定时与排队 | [`ExecuteWithDelay`]({% link lua-api/global-functions/executewithdelay.md %})、[`ExecuteInGameThread`]({% link lua-api/global-functions/executeingamethread.md %})、[`LoopAsync`]({% link lua-api/global-functions/loopasync.md %})、[`NotifyOnNewObject`]({% link lua-api/global-functions/notifyonnewobject.md %}) |
| 输出 | `print(...)`：等价于 `Log.info`，见 [Log]({% link lua-api/mod-env/log.md %}) |

在模组里，`RegisterHook`、`ExecuteWithDelay`、`ExecuteInGameThread`、`LoopAsync`、`NotifyOnNewObject` 带有模组标记：模组被禁用或重新加载时，它注册的东西自动撤销。
