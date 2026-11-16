---
title: Pak 模组
nav_order: 4
---

# Pak 模组

把 `.pak` 文件放进手机上的 `kmod/paks/` 文件夹，游戏启动后 kmod 会自动把它们挂载到引擎里，不需要改游戏的 `Paks` 目录。

```
/sdcard/Android/media/com.android1939.kardsapk/kmod/paks/
```

## 工作方式

- 游戏启动、Lua 主机就绪后（约 5 秒），`kmod/paks/` 里的每个 `.pak` 按文件名排序挂载，每次游戏运行只挂载一次。
- 优先级：kmod 把这些 pak 的 order 设成 100 起（按排序递增），**高于游戏自带的 pak**，所以它们能替换游戏里同路径的资源。文件名排在后面的 pak 优先级更高。
- 挂载发生在游戏已经开始加载资源之后。挂载前已经加载进内存的资源不会被换掉，要重启游戏、让它们在挂载后再加载才会看到效果。
- 只读取，不改动 pak 文件。
- 删除或替换 pak 之后需要**重启游戏**才生效，kmod 不会卸载已挂载的 pak。

## 控制台

```
paks        列出 kmod/paks 里的 pak 和挂载状态（mounted / failed / not mounted）
```

Lua 里也可以直接调用 `kmod.mount_pak(路径)`（返回是否成功），不过一般不需要。

## 限制

- 只支持 KARDS 1.61.27596。引擎里的挂载函数位置是按这个版本写死的，核心会先检查函数开头的指令，不符合就跳过挂载并写日志。
- 包含新资源的 pak（新卡牌等）自带完整的 `AssetRegistry.bin`，多个这样的 pak 同时挂载，只有一个的新资源能被游戏发现。只替换已有资源的 pak（例如换卡图）没有这个问题。
- 两个 pak 替换同一个资源时，只有优先级高的那个生效。需要在多套卡图之间切换，请用 [card_art 示例模组]({{ site.github.repository_url }}/tree/main/examples/card_art)，它在运行时用 PNG 变体切换。
- 如果同时挂载了换卡图的 pak 和 `card_art`，`card_art` 眼里的"原图"就是 pak 里的图。它仍然能工作，但按钮里的选项会和 pak 的图重复。
