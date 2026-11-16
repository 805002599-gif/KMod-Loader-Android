---
title: 游戏线程
nav_order: 1
---

# 游戏线程

kmod 在 `UObject::ProcessEvent` 上装了一个内联钩子。游戏线程每次调用 `ProcessEvent` 时，核心都顺带运行 Lua：

- **Lua 只在游戏线程执行**：模组脚本、钩子回调、定时器回调、控制台指令都在这个线程里，直接读写游戏对象是安全的，不需要加锁。
- **tick 约 20 Hz**：定时器和 `NotifyOnNewObject` 的检查每秒大约执行 20 次，所以 `ExecuteWithDelay` / `LoopAsync` 的精度在 50 ms 量级，不适合做动画。
- **回调要快**：回调同步执行，卡住就等于卡住游戏。需要做重活（比如同步 [`LoadAsset`]({% link lua-api/global-functions/loadasset.md %})）时，注意它可能卡一帧到几帧。
- **没有真正的异步**：`LoopAsync`、`ExecuteInGameThread` 名字像 UE4SS，但在 kmod 里都跑在游戏线程上，只是被排到后面的 tick。
