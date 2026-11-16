---
title: 内置指令
nav_order: 1
parent: 控制台指令
---

# 内置指令

| 指令 | 作用 |
|---|---|
| `help [指令]` | 列出所有指令；带参数时显示该指令的说明和用法 |
| `version` | 加载器版本、构建类型、接口级别、游戏版本 |
| `mods` | 列出已安装的模组、版本和状态 |
| `enable <模组id>` / `disable <模组id>` | 启用或禁用模组并重新加载 |
| `reload` | 重新加载全部模组 |
| `lang [auto\|en\|zh-CN\|...]` | 查看或设置界面语言 |
| `settings <模组id> [键 [值]]` | 列出、读取或修改模组设置 |
| `log [条数]` | 显示 `kmod.log` 末尾若干行（默认 20，最多 200） |
| `find <类名> [上限]` | 列出某个类的存活对象路径（默认 20 个） |
| `props <类名>` / `funcs <类名>` | 列出类的属性或函数。类名不带 `U` / `A` 前缀 |

## settings

- `settings mymod`：列出设置项和当前值。
- `settings mymod scale`：读一项。
- `settings mymod scale 1.5`：写一项。`number` 会按 `min` / `max` 截断，`choice` 只接受声明过的选项，`bool` 接受 `true/false/1/0/on/off`，`string` 会按 `maxlen` 截断。

## find / props / funcs

```
> find PlayerController 2
1 object(s)
/Game/Maps/StartingLevel.StartingLevel:PersistentLevel:BP_PlayerController_C
> props PlayerController
144 props
AcknowledgedPawn : ObjectProperty  (PlayerController)
...
```
