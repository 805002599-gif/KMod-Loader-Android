---
title: 安装
nav_order: 1
---

# 安装

kmod 以打过补丁的 APK 形式发布，不需要 root。你需要官方渠道获得的 KARDS APK。

1. 按[构建加载器]({% link guides/building.md %})生成 `kards_mod.apk`（正式版）或 `kards_mod_dev.apk`（开发版）。
2. 安装。补丁 APK 用你自己的签名，和官方签名不同，所以不能覆盖安装官方版：需要先卸载官方版，或者覆盖同一把签名的旧补丁版（`adb install -r`）。
3. 启动游戏。首次启动会在 `/sdcard/Android/data/com.android1939.kardsapk/files/kmod/` 下创建目录。

{: .warning }
> 换签名后装不上官方版，卸载会清掉应用数据。安装前先备份存档。

不要公开分发补丁后的 APK：里面有游戏本体。

## 目录

```
/sdcard/Android/data/com.android1939.kardsapk/files/kmod/
├── mods/<id>/          # 已安装的模组（文件夹形式）
├── data/<id>/          # 模组的用户数据（设置值）
├── inbox/              # 放 .kmod 包，下次启动或打开 Mods 页时自动安装
├── logs/               # kmod.log 等日志
├── state.json          # 启用列表和语言
└── dev/                # 仅开发版使用
```

用 USB 连接电脑后 `adb push`，或用手机上的文件管理器，都能操作这个目录。

## 确认装好了

- 游戏设置页右下角出现 `kmod 0.2.0 release`（开发版是 `dev`）。
- 设置页左侧多一个 **Mods** 标签。
- `kmod/logs/kmod.log` 第一行是 `=== kmod loader 0.2.0 (release) started ===`。
