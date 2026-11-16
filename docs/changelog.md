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

**未发布的更新**
- `Worker`：独立线程、独立 Lua 状态，消息通信（见 [Worker]({% link lua-api/global-functions/worker.md %})）；`ExecuteAsync`（别名）。
- `FastLoop`，`NotifyOnNewObject` 的快速模式；新对象检测改为比较槽位序号，不再漏掉被复用的槽位。
- `kmod/paks/` 里的 pak 启动后自动挂载，优先级高于游戏自带 pak（见 [Pak 模组]({% link guide/pak-mods.md %})）。
- 控制台指令 `paks`（可开关单个 pak）、`safe`、`unquarantine`、`selftest`。
- **版本抗性**：`GObjects`、`GNames`、`ProcessEvent`、pak 挂载函数改为运行时验证和查找，写死的偏移失效时自动重新定位，找不到时安静地不活动（见[版本支持]({% link concepts/version-support.md %})）。
- **崩溃保护**：记录崩溃时正在运行的模组或 pak，下次启动隔离它；原因不明的连续崩溃进入安全模式（见[崩溃保护]({% link guide/safe-mode.md %})）。
- pak 由核心在引擎就绪时挂载，比之前早约 5 秒。
- `Events`（模组间事件总线）、`Store`（模组的持久化存储）、`Mod.data_dir`。
- UObject 句柄现在也比较槽位序号，对象被销毁、槽位被复用后旧句柄不会指向新对象。

**Lua API**
- 查找：`FindFirstOf`、`FindAllOf`、`FindObject`、`StaticFindObject`、`CreateInvalidObject`。
- 资源：`LoadAsset`（同步加载 pak 内容）；`obj:GetPathName()`。
- 钩子与定时：`RegisterHook`、`UnregisterHook`、`ExecuteWithDelay`、`ExecuteInGameThread`、`LoopAsync`、`NotifyOnNewObject`。
- 模组环境：`Mod`、`Log`、`Settings`、`Lang`、`Console`、`require`、`kmod`。

**工具**
- 控制台指令系统：`help`、`version`、`mods`、`enable`、`disable`、`reload`、`lang`、`settings`、`log`、`find`、`search`、`props`、`funcs`（带参数类型）、`hook` / `unhook` / `hooks`（监听函数调用并打印参数）、`typegen`（生成 LuaLS 类型声明），模组可注册指令。
- 日志：滚动文件日志（保留 5 次运行），原生崩溃处理写入 `FATAL` 回溯。

**数据位置**
- 所有数据放在共享存储的 `Android/media/<包名>/kmod/`：游戏不需要存储权限，文件管理器和 `adb` 放进去的文件游戏都读得到；旧位置的数据首次启动时自动搬迁。

**已知缺口**
- 没有 C++ 模组接口；只有 Android，Windows 版未做。
- 没有一键转储对象 / USMAP / 头文件。
