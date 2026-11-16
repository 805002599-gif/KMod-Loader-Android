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
- **游戏自身的网络**：服务器重定向（`kmod/net/server.json`，运行时修改内存里的服务器地址，不用重打包）；[`Net`]({% link lua-api/mod-env/net.md %})（观察、修改游戏收到的 WebSocket 消息，连接状态，`Net.send`），权限 `game_network`。
- **模组的网络**：[WebSocket 客户端、TCP（可选 TLS）、UDP]({% link lua-api/mod-env/sockets.md %})；`Http.download` 流式下载到文件，带进度。
- 钩子里给字符串参数赋值现在可用（引擎分配的副本）。
- **自定义界面**：[`UI`]({% link lua-api/mod-env/ui.md %})（`UI.window`、`toast`），用游戏自己的控件搭窗口。
- **网络**：[`Http`]({% link lua-api/mod-env/http.md %})（`get`、`post`、`request`），需要 `mod.json` 声明 `permissions: ["network"]`。
- **布局检测与缓存**：用 MobileDumper-7 的检测在游戏进程里取得引擎的结构布局，按游戏版本缓存，和核心里的 SDK 对比；不一致时不加载模组（见[版本支持]({% link concepts/version-support.md %})）。控制台 `layout`。
- **运行时反射布局**：核心的反射读取改走 `reflect.h` 的运行时布局表，检测到的引擎布局会被采用；引擎函数按类名、函数名、参数名调用，不再用 SDK 强类型包装。布局偏移变了不必重编译核心（见[版本支持]({% link concepts/version-support.md %})）。
- **UE4SS 兼容补齐**（见 [UE4SS 兼容]({% link lua-api/global-functions/ue4ss-compat.md %})）：钩子回调的返回值可替换函数返回值；延迟动作句柄一整套（`MakeActionHandle`、`ExecuteInGameThreadWithDelay`、`LoopInGameThread*`、`Pause/Cancel…`）；`RegisterBeginPlay/EndPlay/InitGameState/LoadMap/ProcessConsoleExec` 钩子、`RegisterCustomEvent`、`RegisterCustomProperty`；`StaticConstructObject`、`ForEachUObject`、`FindObjects`、`LoadExport`、线程查询、`DumpAllObjects/Actors`、模组重启/卸载；对象方法 `IsChildOf`、`GetSuperStruct`、`ForEachProperty/Function`、`get`；`TMap`/`TSet` 可读、`TMap` 值可原位修改；委托属性（`Broadcast`、`Execute`、`IsBound`、`GetBindings`、`Clear`）；`FString`/`FText` 属性可写，数组可原位写。
- **原生函数钩子**：蓝图虚拟机直接调用的原生函数也能钩了（参数可读可改），`GNatives` 在运行时从解释器里定位。`Net.on_send`、`Net.on_open` 补齐。
- **蓝图脚本函数钩子**：`RegisterHook` 现在钩在引擎执行蓝图字节码的解释器上，能看到**所有**蓝图函数调用（包括蓝图直接调蓝图，之前完全看不到），参数可读可改，后置回调里能读返回值（见[钩子]({% link concepts/hooks.md %})）。
- 修复：`GetName()` 曾在反射改造后多出实例编号后缀（`ScaleBox_2`），导致 Mods 页找错控件，已恢复。
- **心跳**：用引擎定时器驱动 tick，游戏静止时定时器不再停摆（之前在静止画面上 `ExecuteWithDelay(300)` 可能拖到几十秒）。
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
