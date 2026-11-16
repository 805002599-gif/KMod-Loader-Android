---
title: kmod loader
---

# kmod loader 文档

**kmod loader**（简称 **kmod**）是 KARDS 的 Lua 模组加载器。它不依赖 UE4SS，也不是 pak 模组：模组是你自己写的 Lua 脚本，可以读取和调用游戏里的一切对象（包括 pak 里的内容）。目前只有 Android 版，Windows 版在计划中。

| 版本信息 | |
|---|---|
| 加载器版本 | 0.2.0（`kmod.version`） |
| 接口级别 | 1（`kmod.api`） |
| 适配的游戏版本 | KARDS 1.61（UE 5.6，arm64） |

## 文档目录

1. [快速开始](getting-started.html)：安装加载器，写第一个模组，打包 `.kmod`。
2. [模组包格式](mod-format.html)：`mod.json`、设置项、依赖与版本条件、本地化。
3. [Lua API 参考](lua-api.html)：全局函数、游戏对象的读写与调用、钩子、定时器、加载资源。
4. [控制台指令](console.html)：内置指令和模组注册指令的接口。
5. [调试与日志](debugging.html)：日志文件、开发版控制台、崩溃日志。
6. [构建加载器](building.html)：从源码构建开发版与正式版 APK。

## 它是怎么工作的

```
官方 APK ──(改 SplashActivity，加 System.loadLibrary("kmod"))──▶ 打补丁的 APK
                                                                    │
libkmod.so（加载器壳） ──加载──▶ libkmodcore.so（核心）
                                    │ 内联 hook UObject::ProcessEvent（拿到游戏线程）
                                    ▼
                               Lua 5.4 虚拟机 ──▶ 模组（main.lua）
```

- 不需要 root：重新打包并签名 APK 后安装。
- 核心在游戏线程里运行 Lua，通过游戏自己的反射系统读写属性、调用函数。
- Mods 页直接嵌在游戏的设置界面里。

## 两种构建

| | 开发版（dev） | 正式版（release） |
|---|---|---|
| 悬浮控制台 | 有 | 无 |
| 日志级别 | DEBUG 及以上 | INFO 及以上 |
| 热加载核心 / 覆盖内置脚本 | 支持（`kmod/dev/`） | 不支持 |
| 面向 | 模组作者 | 玩家 |

版本行显示在游戏设置页右下角，如 `kmod 0.2.0 release`。
