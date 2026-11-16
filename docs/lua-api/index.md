---
title: Lua API
nav_order: 5
has_children: true
has_toc: false
---

# Lua API

模组脚本运行在 Lua 5.4 里，在[游戏线程]({% link concepts/game-thread.md %})上执行。每个模组有独立的环境：全局变量互不干扰，但都能读到标准库和下面列出的全局函数。

{: .note }
> 本章的接口都在 Android 版 0.2.0（接口级别 1）上实现。每个页面里标成"未验证"的内容是按代码写的，没有在手机上跑过。

| 章节 | 内容 |
|---|---|
| [模组环境]({% link lua-api/mod-env/index.md %}) | 每个模组都有的表：`Mod`、`Log`、`Settings`、`Lang`、`Console`、`require`、`kmod` |
| [全局函数]({% link lua-api/global-functions/index.md %}) | 查找对象、加载资源、钩子、定时器 |
| [类]({% link lua-api/classes/index.md %}) | `UObject` 包装的方法，钩子的 `params` |
