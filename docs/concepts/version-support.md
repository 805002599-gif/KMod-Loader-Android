---
title: 版本支持
nav_order: 5
---

# 版本支持与地址解析

kmod 要在游戏里找到几样东西：对象表（`GObjects`）、名字表（`GNames`）、`ProcessEvent`，以及 pak 挂载函数。这些地址每个游戏版本都可能变。kmod 不再信任一份写死的偏移，而是在启动时**验证，不对就重新找**。

| 目标 | 做法 |
|---|---|
| `GObjects` | 先检查 SDK 导出时记下的偏移；不是有效的对象数组就扫描引擎的数据段，找符合对象数组结构的位置 |
| `GNames` | 同上；扫描时找"第一个名字块的开头是 `None`"的结构，并用 SDK 的名字解码再确认 |
| `ProcessEvent` | 直接读任意 `UObject` 的虚表第 77 槽（虚表下标在同一引擎版本里不变），不依赖代码地址 |
| `GWorld` | 不再使用，改由引擎对象的 `GameViewport` 取得 |
| pak 挂载 | 在引擎里找日志字符串 `Mounting pak file: %s`，再在内存里找绑定了对应函数的委托对象，取出 `FPakPlatformFile`；挂载 worker 通过它读参数结构的指令特征识别 |

日志（`kmod/logs/kmod.log`）里能看到用的是哪种方式：

```
I/resolver: GObjects: dumped offset 0xa1d5fc0 is valid
I/resolver: GNames: found by scanning at 0xa17c000
I/core: pak layer: mount fn 0x4dc0218 worker 0x4dc30f0 pak file 0x4fedc96000
```

## 找不到时

- 对象表或名字表找不到：kmod 写一条错误日志（`this game build is not supported`），**保持不活动**，游戏照常运行，不加载任何模组。
- 只有 pak 层找不到：写警告，`kmod/paks` 不挂载，其他功能不受影响。

## 仍然依赖的东西

地址解析只解决"函数和全局变量在哪"。SDK 里的类布局（例如 `UObject` 的成员偏移）仍是导出那一刻的，游戏更新改了这些布局时，需要用 Dumper-7 重新导出 SDK，并运行一次 `tools/patch_sdk.py`（把 SDK 里写死的偏移改成运行时可写的变量），再重新构建核心。

## 开发用开关

在开发版里，`kmod/dev/force_scan` 文件存在时，核心会忽略导出的偏移，强制走扫描路径，用来测试扫描逻辑。
