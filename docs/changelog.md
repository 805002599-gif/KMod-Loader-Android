---
title: 更新日志
nav_order: 8
---

# 更新日志

## 0.2.0

首个有文档的版本（Android，KARDS 1.61）。

**加载器**
- 无 root：重新打包并签名 APK，`SplashActivity` 静态初始化里加载 `libkmod.so`，再加载核心。
- 内联钩子 `UObject::ProcessEvent`，Lua 5.4 运行在游戏线程上。
- 开发版 / 正式版两种构建；开发版有悬浮控制台、热加载核心和脚本。

**模组**
- `.kmod` 包格式（format 1）：`mod.json`、`main.lua`、`lang/*.json`；包和用户数据分开。
- 设置项：`bool`、`number`、`choice`、`string`。
- 依赖：`id` 加版本条件（`>=`、`>`、`<=`、`<`、`=`，逗号分隔），保留 id `kmod`（加载器）和 `kards`（游戏，只取前两段版本）。
- 本地化：12 种语言标签，回退链 `zh-CN → zh → en → 默认文字 → 键名`。
- 脚本报错自动禁用：钩子、定时器、新对象回调累计 10 次错误。
- 游戏内 Mods 页：嵌在设置界面，跟随游戏的 UI 缩放，显示名称、说明、版本、状态、启用开关和设置项。

**Lua API**
- 查找：`FindFirstOf`、`FindAllOf`、`FindObject`、`StaticFindObject`、`CreateInvalidObject`。
- 资源：`LoadAsset`（同步加载 pak 内容）；`obj:GetPathName()`。
- 钩子与定时：`RegisterHook`、`UnregisterHook`、`ExecuteWithDelay`、`ExecuteInGameThread`、`LoopAsync`、`NotifyOnNewObject`。
- 模组环境：`Mod`、`Log`、`Settings`、`Lang`、`Console`、`require`、`kmod`。

**工具**
- 控制台指令系统：`help`、`version`、`mods`、`enable`、`disable`、`reload`、`lang`、`settings`、`log`、`find`、`search`、`props`、`funcs`（带参数类型）、`hook` / `unhook` / `hooks`（监听函数调用并打印参数），模组可注册指令。
- 日志：滚动文件日志（保留 5 次运行），原生崩溃处理写入 `FATAL` 回溯。

**已知缺口**
- 没有 C++ 模组接口；只有 Android，Windows 版未做。
- 没有一键转储对象 / USMAP / 头文件。
