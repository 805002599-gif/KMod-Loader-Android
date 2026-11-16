---
title: 快速开始
---

# 快速开始

## 1. 安装加载器

kmod 以打过补丁的 APK 形式发布，不需要 root。你需要官方渠道获得的 KARDS APK，然后：

1. 按 [构建加载器](building.html) 生成 `kards_mod.apk`（正式版）或 `kards_mod_dev.apk`（开发版）。
2. 卸载或覆盖安装：补丁 APK 用自己的签名，和官方签名不同，所以需要先卸载官方版再装（或直接 `adb install -r` 覆盖同签名的旧补丁版）。
3. 启动游戏。首次启动会在 `/sdcard/Android/data/com.android1939.kardsapk/files/kmod/` 下创建目录。

> 安装前先备份存档：改签名后不能覆盖安装官方版，卸载会清掉应用数据。

## 2. 目录

```
/sdcard/Android/data/com.android1939.kardsapk/files/kmod/
├── mods/<id>/          # 已安装的模组（文件夹形式）
├── data/<id>/          # 模组的用户数据（设置值）
├── inbox/              # 放 .kmod 包，下次启动或打开 Mods 页时自动安装
├── logs/               # kmod.log 等日志
├── state.json          # 启用列表和语言
└── dev/                # 仅开发版使用
```

用 USB 连接电脑、`adb push` 或手机上的文件管理器都能操作这个目录。

## 3. 第一个模组

在 `mods/hello/` 下建两个文件。

`mod.json`：

```json
{
  "format": 1,
  "id": "hello",
  "name": "Hello",
  "version": "1.0.0",
  "depends": ["kmod>=0.2.0", "kards>=1.61"]
}
```

`main.lua`：

```lua
Log.info("hello from " .. Mod.id .. " v" .. Mod.version)

-- 每次玩家控制器重启（进入新场景）时打印一行
RegisterHook("/Script/Engine.PlayerController:ClientRestart", function(self, params)
  Log.info("controller restarted: " .. self:GetName())
end)
```

> 钩子回调的参数是 `(self, params)`：`self` 是被调用的游戏对象，`params` 按名字访问函数参数。详见 [Lua API](lua-api.html#registerhook)。

然后：

1. 编辑 `state.json`，把 `"hello"` 加进 `enabled`，或者打开游戏设置页的 **Mods** 标签，勾选启用。
2. 重启游戏（Mods 页修改启用状态后会立即重新加载模组）。
3. 看 `logs/kmod.log`，应能看到 `I/hello: hello from hello v1.0.0`。

## 4. 打包成 .kmod

把模组文件夹里的**内容**（不要多套一层目录）压成 zip，扩展名改成 `.kmod`：

```bash
cd hello && zip -r ../hello-1.0.0.kmod .
```

把它放进 `kmod/inbox/`，下次启动或打开 Mods 页时自动安装。安装失败的包会被移到 `inbox/failed/`，旁边的 `.txt` 说明原因。同一个 `id` 的新包会整体替换旧包，用户设置和启用状态保留。

## 5. 下一步

- 想要设置项、本地化、依赖：[模组包格式](mod-format.html)
- 想操作游戏对象：[Lua API 参考](lua-api.html)
- 出问题了：[调试与日志](debugging.html)
