---
title: 控制台指令
---

# 控制台指令

kmod 有一套指令系统：内置指令用来查看和管理模组，模组可以用 `Console.register` 注册自己的指令。

> 指令系统在开发版和正式版里都存在，但**输入界面只有开发版有**（悬浮控制台，见 [调试与日志](debugging.html)）。正式版里模组仍能注册指令，只是玩家没有地方输入；模组可以用 `Console.exec` 在自己的脚本里触发指令。

## 怎么输入

在开发版控制台里输入一行：

- 第一个词是已注册的指令名，就按指令执行：`mods`、`greet "Mr Smith"`。
- 行首加 `/` 强制按指令执行：`/mods`。指令不存在时会提示 `unknown command`，不会当 Lua 去跑。
- 其他情况照旧当 Lua 表达式或语句执行。
- 指令名后面紧跟 Lua 语法（`=`、`.`、`(`、`:`、`[`）时，当 Lua 处理，所以 `mods = 1` 不会误触发 `mods` 指令。

参数按空格分隔，可以用双引号或单引号把带空格的内容括起来，引号里用反斜杠转义：`greet "Mr \"X\" Smith"` 得到一个参数。

## 内置指令

| 指令 | 作用 |
|---|---|
| `help [指令]` | 列出所有指令；带参数时显示该指令的说明和用法 |
| `version` | 加载器版本、构建类型、接口级别、游戏版本 |
| `mods` | 列出已安装的模组、版本和状态 |
| `enable <模组id>` / `disable <模组id>` | 启用或禁用模组并重新加载 |
| `reload` | 重新加载全部模组 |
| `lang [auto\|en\|zh-CN\|...]` | 查看或设置界面语言 |
| `settings <模组id> [键 [值]]` | 列出、读取或修改模组设置。`number` 会按 `min`/`max` 截断，`choice` 只接受声明过的选项，`bool` 接受 `true/false/1/0/on/off` |
| `log [条数]` | 显示 `kmod.log` 末尾若干行（默认 20，最多 200） |
| `find <类名> [上限]` | 列出某个类的存活对象路径（默认 20 个） |
| `props <类名>` / `funcs <类名>` | 列出类的属性或函数。类名不带 `U`/`A` 前缀 |

## 给模组注册指令

模组环境里有 `Console` 表：

```lua
Console.register("greet", function(args, ctx)
  local who = args[1] or "world"
  return "hello " .. who          -- 返回字符串会被打印
end, { help = "say hello", usage = "greet [name]" })
```

### Console.register(name, handler, info)

- `name`：小写字母开头，只能含小写字母、数字、`_`、`-`、`.`。
- `handler(args, ctx)`：
  - `args` 是参数数组（字符串）。
  - `ctx.line` 是不含 `/` 前缀的原始整行，`ctx.print(...)` 把内容打印到控制台。
  - 返回字符串则打印；返回 `nil` 什么都不打印。
  - 抛出错误时，控制台显示 `[error] 名字: 原因`，同时在日志里留一条警告。指令出错**不计入**模组的自动禁用错误数。
- `info`：一个字符串（作为说明），或 `{ help = "...", usage = "..." }`。
- 返回 `true`；失败返回 `false, 原因`，同时写一条警告到日志。失败的情形：名字不合法、`handler` 不是函数、名字已被内置指令或其他模组占用。同一个模组可以重复注册同名指令来覆盖自己的。

指令不会自动带模组前缀；想避免冲突，建议用 `模组id.指令名` 或 `模组id-指令名` 的形式。

### 其他接口

| 函数 | 说明 |
|---|---|
| `Console.unregister(name)` | 取消自己注册的指令，成功返回 `true` |
| `Console.exec(line)` | 像在控制台里输入一样执行一行，返回是否被当作指令处理 |
| `Console.print(...)` | 打印到控制台 |

模组被禁用或重新加载时，它注册的指令会自动撤销。

## 示例：一个管理指令

```lua
local scale = Settings.get("scale")

Console.register("mymod.scale", function(args)
  if not args[1] then return "scale = " .. Settings.get("scale") end
  local v = tonumber(args[1])
  if not v then return "expected a number" end
  Settings.set("scale", v)
  return "scale set to " .. v
end, { help = "show or set the scale", usage = "mymod.scale [value]" })
```

输入 `mymod.scale 1.5` 即可修改。
