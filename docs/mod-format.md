---
title: 模组包格式
---

# 模组包格式（format 1）

一个包 = 一个文件夹；本地化可选，不写也能用。

## 1. 包的样子

```
hello_mod/                 # 文件夹名 = 模组 id
├── mod.json               # 清单（必需）
├── main.lua               # 入口（必需）
├── lang/                  # 本地化（可选）
│   ├── en.json
│   └── zh-CN.json
└── ...                    # 其余 Lua 模块、数据文件，随意
```

- **分发形式**：把文件夹里的内容打成 zip，扩展名改成 `.kmod`（根目录直接放上面这些文件，不要再套一层文件夹）。
- **安装**：解压到 `kmod/mods/<id>/`；直接放一个文件夹进去效果一样。
- **自动安装**：把 `.kmod` 放进 `kmod/inbox/`，下次启动或进入 Mods 页时自动解压并删除。

## 2. 运行时目录（包和用户数据分开）

```
kmod/
├── mods/<id>/              # 安装的包，视为只读；升级 = 整个替换
├── data/<id>/settings.json # 用户在 Mods 页里改的设置值
├── state.json              # {"enabled":["hello_mod"], "language":"auto"}
├── inbox/                  # 待安装的 .kmod（失败的移到 inbox/failed/）
├── logs/                   # 日志
└── dev/                    # 开发版专用
```

升级或重装模组不会丢用户设置和启用状态。

## 3. mod.json

```json
{
  "format": 1,
  "id": "hello_mod",
  "name": "Hello Mod",
  "version": "1.0.0",
  "author": "kmod",
  "description": "A demo mod.",
  "depends": ["kmod>=0.2.0", "kards>=1.61"],
  "settings": [
    {"id":"verbose","type":"bool","label":"Verbose log","default":false},
    {"id":"scale","type":"number","label":"Scale","min":0.5,"max":2,"step":0.25,"default":1},
    {"id":"mode","type":"choice","label":"Mode","options":["low","medium","high"],"default":"medium"}
  ]
}
```

| 字段 | 必需 | 说明 |
|---|---|---|
| `format` | 是 | 包格式版本，目前是 1 |
| `id` | 是 | 小写字母、数字、下划线（`^[a-z0-9_]+$`）；必须和文件夹名一致 |
| `version` | 是 | 版本号，供依赖检查和显示 |
| `name` | 否 | 默认显示名（通常英文）；缺省用 id |
| `author` | 否 | 作者 |
| `description` | 否 | 说明，显示在 Mods 页该模组的版本行上方；有 `lang` 翻译时用翻译 |
| `api` | 否 | 需要的加载器接口级别，高于当前（`kmod.api`）就拒绝加载。新模组建议改用 `kmod>=x.y.z` |
| `depends` | 否 | 依赖列表，见下 |
| `settings` | 否 | 设置项，类型 `bool`、`number`、`choice`、`string` |

### 依赖与版本条件

```json
"depends": ["kmod>=0.2.0", "kards>=1.61", "hello_mod>=1.0.0,<2.0.0", "util_mod"]
```

每个元素是 `id` 加可选的版本条件：

- 运算符：`>=` `>` `<=` `<` `=`（`==` 同义）。多个条件用逗号分隔，须全部满足；不写条件表示任意版本。
- 版本按 `x.y.z` 三段数字逐段比较，缺省的段补 0（`1.101` 大于 `1.61`，不是按小数比）。后缀（如 `-beta`）不识别，会被忽略。
- 先加载依赖，再加载模组本身；依赖不存在、未启用或有错误，则本模组不加载。

**保留 id：**

| id | 含义 | 版本取值 |
|---|---|---|
| `kmod` | 加载器本身 | `kmod.version`（如 `0.2.0`） |
| `kards` | 游戏本体 | 游戏版本号 `x.xx.xxxx` 只取第二个点之前，如 `1.61`、`1.101`（`kmod.game`） |

保留 id 不需要安装，也不参与加载顺序。读不到游戏版本时，`kards` 的条件会被忽略并在日志里写一条警告，模组照常加载。

**不满足时**：模组不加载，Mods 页和日志里显示原因：

| 情形 | 状态文字 |
|---|---|
| 加载器或游戏版本不符 | `requires kmod>=9.0.0 (have 0.2.0)`、`requires kards>=1.101 (have 1.61)` |
| 依赖的模组版本不符 | `dependency version: hello_mod>=2.0.0 required (have 1.0.0)` |
| 依赖缺失 / 未启用 / 有错误 | `missing dependency: <id>` |
| 依赖自己加载失败 | `dependency failed: <id>` |
| 循环依赖 | `dependency cycle` |
| 写法错误（如 `"kmod>>1"`） | `bad dependency ...`，视为模组错误 |

`.kmod` 安装时同样检查保留 id 的条件和写法，不通过的包移到 `inbox/failed/`，旁边的 `.txt` 写明原因。

### 设置项

| 类型 | 字段 | 说明 |
|---|---|---|
| `bool` | `default` | 勾选框 |
| `number` | `min` `max` `step` `default` | 滑块，范围任意（已实测 0–100 步长 5） |
| `string` | `default` `maxlen` | 文本输入框（游戏自带的输入控件，点它会弹出系统键盘）。提交（失去焦点或回车）时保存；`maxlen` 可选，超出会被截断。值不是字符串时退回默认值 |
| `choice` | `options` `default` | 下拉框。`options` 是**稳定的值**（如 `"low"`），保存的也是这个值；显示文字走本地化，所以换语言不会丢设置 |

脚本里用 `Settings.get("scale")`、`Settings.set(...)`、`Settings.on_change(fn)` 读写，见 [Lua API](lua-api.html#settings)。

## 4. 本地化

### 文件

`lang/<语言>.json`，UTF-8，**扁平的键值表**：

```json
{
  "name": "测试模组",
  "description": "一个演示模组。",
  "settings.verbose.label": "详细日志",
  "settings.mode.label": "模式",
  "settings.mode.options.low": "低",
  "settings.mode.options.medium": "中",
  "settings.mode.options.high": "高",
  "greeting": "你好，{name}！"
}
```

### 保留键（界面自动使用）

| 键 | 覆盖 |
|---|---|
| `name`、`description` | 模组名、说明 |
| `settings.<id>.label` | 设置项名称 |
| `settings.<id>.options.<值>` | 下拉选项的显示文字 |

`mod.json` 里写的 `name`、`label` 就是**默认文字**。没有翻译文件时直接显示它；有翻译时，按键覆盖。其余键随便起，给脚本用。本地化只管模组自己的文字，不能替换游戏文本。

### 语言选择与回退

语言标签：`en`、`zh-CN`、`zh-TW`、`ja`、`ko`、`ru`、`de`、`fr`、`es`、`pt`、`pl`、`it`（正好是游戏自己的 12 种语言）。

- 当前语言默认跟随游戏语言设置（`state.json` 里 `"language":"auto"`），也可在 Mods 页顶部的语言下拉手动指定。
- 查找顺序：`zh-CN` → `zh` → `en` → `mod.json` 里的默认文字 → 键名本身。只写 `en.json` 和 `zh-CN.json` 就够了。

脚本里用 `Lang.t(key, vars)`，见 [Lua API](lua-api.html#lang)。

## 5. 加载流程与状态

启动时：读 `state.json` → 迁移旧格式 → 安装 `inbox` 里的 `.kmod` → 扫描并校验 → 按依赖排序 → 运行已启用的模组。进入 Mods 页时也会安装 `inbox`，有变化就重新加载。

Mods 页里每个模组显示一个状态：

| 状态 | 含义 |
|---|---|
| `loaded` / `disabled` | 正常 |
| `error: ...` | 清单有问题（缺 `main.lua`、`id` 与文件夹不符、版本条件不满足等） |
| `missing dependency` / `dependency version` / `dependency failed` / `dependency cycle` | 依赖问题，见上 |
| `load error` | `main.lua` 运行时出错，错误和调用栈在日志里 |
| `disabled after N script errors: ...` | 运行中钩子、定时器或新对象回调累计出错 10 次，被自动禁用（见下） |

**自动禁用**：模组在钩子、`ExecuteWithDelay` / `LoopAsync`、`NotifyOnNewObject` 的回调里累计抛出 10 次错误（同一次运行内累计），加载器会把它禁用（写入 `state.json`）并重新加载全部模组。原因写进日志，并显示在 Mods 页的状态里。用户重新勾选启用后，该模组会重新开始计数。

## 6. 从旧布局迁移

加载器会自动兼容并迁移：

| 旧布局 | 新格式 | 说明 |
|---|---|---|
| `Scripts/main.lua` | `main.lua` | 没有根目录 `main.lua` 时仍读 `Scripts/main.lua`，并警告 |
| `<mod>/enabled.txt` | `state.json` 的 `enabled` | 首次启动自动迁移，然后删除旧文件 |
| `<mod>/settings.json` | `data/<id>/settings.json` | 首次启动自动搬移 |
| `choice` 保存选项文字 | 保存稳定值 | 旧值等于某个选项时沿用 |
| `mod.json` 无 `id` | 必须有 `id` | 缺省时取文件夹名并警告 |
