---
title: 加载流程与状态
nav_order: 4
parent: 模组包格式
---

# 加载流程与状态

Mods 页里每个模组显示一个状态：

| 状态 | 含义 |
|---|---|
| `loaded` / `disabled` | 正常 |
| `error: ...` | 清单有问题（缺 `main.lua`、`id` 与文件夹不符、版本条件不满足等） |
| `missing dependency` / `dependency version` / `dependency failed` / `dependency cycle` | 依赖问题，见[依赖与版本条件]({% link mod-format/dependencies.md %}) |
| `load error` | `main.lua` 运行时出错，错误和调用栈在日志里 |
| `disabled after N script errors: ...` | 运行中累计出错 10 次，被自动禁用，见[模组生命周期]({% link concepts/lifecycle.md %}) |

加载顺序和重新加载的细节见[模组生命周期]({% link concepts/lifecycle.md %})。
