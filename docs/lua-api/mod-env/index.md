---
title: 模组环境
nav_order: 1
parent: Lua API
has_children: true
has_toc: false
---

# 模组环境

每个模组的 `main.lua` 里直接可用：

| 名字 | 说明 |
|---|---|
| [`Mod`]({% link lua-api/mod-env/mod.md %}) | 模组信息 |
| [`Log`]({% link lua-api/mod-env/log.md %}) | 日志 |
| [`Settings`]({% link lua-api/mod-env/settings.md %}) | 用户设置 |
| [`Lang`]({% link lua-api/mod-env/lang.md %}) | 本地化 |
| [`Console`]({% link console/register.md %}) | 注册控制台指令 |
| [`require`]({% link lua-api/mod-env/require.md %}) | 加载本模组内的 Lua 文件 |
| [`kmod`]({% link lua-api/mod-env/kmod.md %}) | 加载器信息 |
| `print(...)` | 等价于 `Log.info` |
