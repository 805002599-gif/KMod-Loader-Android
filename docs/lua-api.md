---
title: Lua API 参考
---

# Lua API 参考

模组脚本运行在 Lua 5.4 里，在游戏线程上执行。每个模组有独立的环境：全局变量互不干扰，但都能读到标准库和下面列出的全局函数。

> 本页的接口都在 Android 版 0.2.0（接口级别 1）上实现并测试过。

## 1. 模组环境

每个模组的 `main.lua` 里直接可用：

| 名字 | 说明 |
|---|---|
| `Mod.id` `Mod.name` `Mod.version` `Mod.dir` | 模组信息；`Mod.path("a/b.dat")` 拼出包内文件的完整路径 |
| `Log` | 日志，见 [日志](#log) |
| `Settings` | 用户设置，见 [Settings](#settings) |
| `Lang` | 本地化，见 [Lang](#lang) |
| `Console` | 注册控制台指令，见 [控制台指令](console.html) |
| `require(name)` | 读取 `<模组目录>/<name>.lua`（`.` 当作目录分隔符），结果会缓存 |
| `print(...)` | 等价于 `Log.info` |
| `kmod` | 加载器信息，见 [kmod 表](#kmod) |

### Log

```lua
Log.debug("...")   -- 仅开发版写入文件
Log.info("...")
Log.warn("...")
Log.error("...")
```

参数可以是任意值，会用制表符连接。日志写到 `kmod/logs/kmod.log`，行首标签是模组 id，见 [调试与日志](debugging.html)。

### Settings

读写用户在 Mods 页里看到的设置项（在 `mod.json` 的 `settings` 里声明）。

```lua
local scale = Settings.get("scale")            -- 值的类型：bool → boolean，number → number，choice → string
Settings.set("scale", 1.5)                     -- 写入并保存，同时触发 on_change；不校验范围，调用方自己保证值合法
Settings.on_change(function(id, value)         -- 用户改动设置后触发
  Log.info(id .. " = " .. tostring(value))
end)
```

### Lang

```lua
Lang.t("greeting", { name = "Bob" })   -- 按当前语言查 lang/*.json，{name} 被替换；缺键返回键名
Lang.current()                         -- "zh-CN"
Lang.on_change(function(tag) end)      -- 游戏语言或手动选择变化时触发
```

查找顺序和语言标签见 [模组包格式](mod-format.html#4-本地化)。

### kmod

只读的加载器信息：

| 字段 | 说明 |
|---|---|
| `kmod.version` | 加载器版本，如 `"0.2.0"` |
| `kmod.api` | 接口级别，整数 |
| `kmod.flavor` | `"dev"` 或 `"release"` |
| `kmod.game` | 游戏版本，只含前两段，如 `"1.61"`；读不到时为 `nil` |
| `kmod.game_version` | 完整的游戏版本，如 `"1.61.27596"`；读不到时为 `nil` |
| `kmod.max_errors` | 自动禁用的错误次数阈值（10） |
| `kmod.version_satisfies(have, cond)` | 版本比较，如 `kmod.version_satisfies("1.101", ">=1.61")` → `true` |
| `kmod.paths` | 路径表：`files`、`mods`、`dev`、`logs` |

开发版额外有 `kmod.debug_crash("segv" | "abort")`，用于测试崩溃日志。

## 2. 游戏对象

### 查找

| 函数 | 说明 |
|---|---|
| `FindFirstOf(className)` | 找某个类的第一个实例（不含默认对象 CDO），没有则返回 `nil` |
| `FindAllOf(className)` | 返回该类所有实例的数组 |
| `FindObject(className, name)` | 按类名和对象短名找 |
| `StaticFindObject(path)` | 找已加载的对象。`path` 可以是 `"/Script/Engine.Default__Object"`、`"WBP_Foo_C"` 这样的完整或尾部路径；尾部必须从 `.` 边界开始匹配 |
| `LoadAsset(path)` | 同步加载资源，见 [加载资源](#loadasset) |
| `CreateInvalidObject()` | 返回一个无效对象，用于初始化变量 |

类名不带 `U` / `A` 前缀（如 `"PlayerController"`、`"WBP_NUI_MasterCheckbox_C"`）。

### 对象的读写

`FindFirstOf` 等返回的是 UObject 包装：

```lua
local pc = FindFirstOf("PlayerController")
if pc and pc:IsValid() then
  print(pc:GetName(), pc:GetClass():GetName())
  local pawn = pc.Pawn                  -- 读属性：直接用名字
  pc.bShowMouseCursor = true            -- 写属性
  pc:ClientIgnoreLookInput(false)       -- 调用游戏函数：obj:函数名(参数...)
end
```

- 读属性、写属性、调用函数都按 UE 的反射名字，和游戏蓝图里的名字一致。
- 对象指针属性返回另一个 UObject 包装；字符串、数值、布尔直接转成 Lua 值；结构体和数组也会转成 Lua 值，个别复杂类型（如委托、Map、Set）暂未完整支持，用前先在开发版控制台试一下。
- 函数的返回值：先是返回值，再是各个输出参数，按声明顺序依次返回。
- 对象被销毁（切场景、被垃圾回收）后，旧引用会变成无效：用 `obj:IsValid()` 检查。访问无效对象的属性会抛 Lua 错误，所以长期保存引用时要先检查。

方法：

| 方法 | 说明 |
|---|---|
| `IsValid()` | 对象是否还活着（对无效对象也可调用） |
| `GetName()` / `GetFullName()` | 短名 / 带类名的名字。**注意**：这两个不含包路径 |
| `GetPathName()` | 带包路径的完整路径，如 `/Game/UI/Foo.Foo_C`，可直接交给 `LoadAsset` |
| `GetClass()` / `GetOuter()` | 类对象 / 外层对象 |
| `IsA(className)` | 类型判断 |
| `GetPropertyValue(name)` / `SetPropertyValue(name, v)` | 显式读写属性（属性名和方法重名时用） |
| `CallFunction(name, ...)` | 显式调用函数 |
| `GetProperties()` / `GetFunctions()` | 列出属性 / 函数，调试用 |
| `GetAddress()` | 对象地址（整数） |

> 写 `FText`、`FString` 之类带文本的属性时，只能通过函数（如 `SetText`）设置；直接给对象的 `FString` 属性赋值不支持。

### LoadAsset

```lua
local cls, err = LoadAsset("/Game/Blueprints/CrossPlatformUI/NUI_Widgets/WBP_NUI_MasterSlider.WBP_NUI_MasterSlider_C")
if not cls then Log.warn(err) end
```

- 同步加载任意资源（包括 pak 里的内容），已经加载的直接返回同一个对象。已在 Android 上测试：加载已载入的蓝图类和贴图返回同一对象，加载 pak 里的 DataTable 成功，路径不存在时返回 `nil` 和原因。
- 路径写 `包路径.对象名`。蓝图类要在对象名后加 `_C`。没有 `.` 时，加载器会把包名最后一段补成对象名。
- 失败时返回 `nil` 和原因字符串。
- 要拿到路径，在游戏里已有对象上调用 `obj:GetPathName()` 最方便；其余可以用 UE 的资源路径（`/Game/...`、`/Engine/...`）。
- 同步加载可能卡顿一帧到几帧，别在每帧调用。

## 3. 钩子与定时器

### RegisterHook

```lua
local id = RegisterHook("/Script/Engine.PlayerController:ClientRestart",
  function(self, params)        -- 函数执行前
    Log.info("before " .. self:GetName())
  end,
  function(self, params)        -- 函数执行后（可选）
    Log.info("after")
  end)
UnregisterHook(id)
```

- 路径格式 `包路径.类名:函数名`；蓝图类的函数用类名加 `_C`。
- 回调参数 `(self, params)`：`self` 是被调用的对象；`params` 按名字读写函数参数，如 `params.NewPawn`。写前置回调的 `params` 可以改变传给函数的值。
- 回调在游戏线程里同步执行，要尽量快；出错只会记入日志，不影响游戏继续运行。
- 模组被禁用或重新加载时，该模组注册的钩子自动撤销。

### 定时与排队

```lua
ExecuteWithDelay(2000, function() ... end)      -- 2 秒后执行一次
ExecuteInGameThread(function() ... end)          -- 排到下一次游戏线程 tick 执行
LoopAsync(1000, function() ... return false end) -- 每秒执行；返回 true 则停止
NotifyOnNewObject("WBP_Foo_C", function(obj) ... end)  -- 新建该类对象时回调
```

- `LoopAsync` 的回调如果抛错，这个循环会停止（不再重复），错误照常计数。
- 定时精度约 50 ms（核心 ~20 Hz 轮询），不适合做动画。
- 这四个函数在模组环境里带有模组标记，模组被禁用或重新加载时自动清理。

## 4. 错误处理

- 钩子、定时器、新对象回调里的错误会计入该模组的错误数，累计 10 次自动禁用，见 [模组包格式](mod-format.html#5-加载流程与状态)。

- `main.lua` 加载出错：状态显示 `load error`，错误和调用栈写进日志；其他模组不受影响。
- 钩子、定时器、回调里的错误会写进日志，不会停止后续调用（但会计数，见上）。
- 脚本里要自己容错就用 `pcall`；访问可能失效的游戏对象时尤其需要。

## 5. 做界面

Mods 页本身就是用这套 API 实现的（见 `core/lua/modmanager.lua`）。要点：

- 用游戏自带的 `WBP_NUI_*` 控件（`WBP_NUI_MasterCheckbox_C`、`MasterSlider_C`、`MasterDropdown_C`、`MasterTextBlock_C` 等），朴素的 UMG 默认控件不会渲染。
- 加到屏幕用 `AddToViewport`，不要用 `AddToPlayerScreen`；文字要在 `AddChild` 之后再 `SetText`。
- **不要**实例化游戏的整页（如 `W_OptionsSettings_C`）来克隆：它们会把原页面的内容槽（NamedSlot）里的控件抢走，原界面就会缺东西。只读取样式，自己搭一个新页面。
- 调试界面可用开发版的 `kmod.tree(widget, depth)` / `kmod.print_tree(widget, depth)` 打印控件树。
