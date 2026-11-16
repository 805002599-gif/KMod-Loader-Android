---
title: 钩子
nav_order: 3
---

# 钩子

`RegisterHook` 挂在游戏的 `UFunction` 上。因为 kmod 钩的是 `ProcessEvent`，所以凡是通过 `ProcessEvent` 调用的函数（蓝图函数、蓝图事件、大部分 Blueprint 可调用的原生函数）都能被挂上。

## 触发时机

- 前置回调（第 2 个参数）在函数执行**之前**触发，后置回调（第 3 个参数，可选）在函数执行**之后**触发。
- 前置回调里改 `params` 可以改变传给函数的参数值；后置回调里能读到输出参数。
- 回调的返回值被忽略：**不能**通过返回值改写函数的返回值，也不能取消调用。

## 函数必须已经存在

`RegisterHook` 注册时要在内存里找到这个 `UFunction`，找不到会抛错 `RegisterHook: function not found: ...`。所以挂蓝图函数时，它所在的类必须已经加载。如果类还没加载，可以先 [`LoadAsset`]({% link lua-api/global-functions/loadasset.md %}) 把它加载进来，或者用 [`NotifyOnNewObject`]({% link lua-api/global-functions/notifyonnewobject.md %}) 等它出现。

## 不能挂的

- 委托函数、纯原生且不经过 `ProcessEvent` 的函数，钩子不会触发。
- 对每帧调用上百次的函数（如 `Tick`）要特别小心：回调在游戏线程里同步执行。

## 清理

钩子带有模组标记：模组被禁用或重新加载时自动撤销，不需要手动 `UnregisterHook`。

## 出错

回调里的错误写进日志，不会影响游戏和其他钩子；但会计入该模组的错误数，累计 10 次模组会被自动禁用，见[模组生命周期]({% link concepts/lifecycle.md %})。
