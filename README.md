# cliplist
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](https://www.gnu.org/licenses/gpl-3.0)
[![Qt](https://img.shields.io/badge/Qt-6-green.svg)](https://www.qt.io/)
[![Meson](https://img.shields.io/badge/Meson-%E2%89%A51.3-orange.svg)](https://mesonbuild.com/)

一个基于 **Qt6** 的剪贴板历史列表 demo，把 CopyQ 的「剪切板列表渲染」链路抽取成一个独立、可复用的小组件：用 `QTextDocument` 在每一行里把 HTML 富文本渲染进 `QListWidget`，并附带行号栏、右侧标签/固定标记、超长折叠、链接点击等交互。

> 本仓库只依赖 Qt Widgets/GUI/Core，不引用 CopyQ 的 Theme / ItemFactory / ItemWidget / ClipboardModel / Platform* / Option / Settings 等模块，也不含 `.ui` 文件与插件机制，可直接丢进任意 Qt6 工程。

---

## 特性

- **HTML 富文本渲染**：每条记录由 `QTextDocument` 排版，支持内联样式、超链接、表格、代码块等。
- **纯文本到 HTML 的等价实现**：自动转义 HTML 元字符、保留换行/制表符/连续空格、识别 URL 并转成超链接（对应 CopyQ `ItemRender::renderAsHtml` 的纯文本分支）。
- **文档缓存与按宽重排**：每行缓存一份 `QTextDocument`，视图宽度变化时重排并通知行高变化，避免每帧重新解析。
- **行号栏**：可选、起始编号可调，宽度随最大行号位数自动变化。
- **超长折叠 + 渐变遮罩**：超过最大高度时底部渐隐，点击遮罩区域即可展开/收起。
- **正文超链接点击**：`editorEvent` 命中链接时发出 `linkActivated` 信号，交由上层打开。
- **右侧 badge**：
  - 用户自定义 **标签**（例如「重要」「链接」）显示在右上角；
  - **固定条目** 显示为无文字、无圆角的淡灰色矩形，并跳过超限淘汰。
- **去重与置顶**：内容哈希（FNV-1a）相同则移动至队首，而非新增。
- **从 `QMimeData` 追加**：自动提取 `text/plain`、`text/html`、`text/uri-list` 与图片格式，其余 MIME 原样保留。
- **过滤、右键菜单、快捷键**：行隐藏过滤、复制、复制为纯文本、设置标签、删除、清空等。
- **轻量独立**：框架无关、无单例、无全局状态，作为组件可单独测试与复用。

---

## 项目结构

```
.
├── main.cpp                    # 示例入口：QApplication + Cliplist
├── cliplist.{h,cpp}            # QMainWindow，演示插入示例记录
├── htmlclipboardlist.{h,cpp}   # 核心：HtmlClipboardList + HtmlItemDelegate
├── htmlclipboardbadges.{h,cpp} # 右侧 badge 的布局与绘制辅助
└── meson.build                 # Meson 构建脚本（moc + Qt6 Widgets）
```

### 组件职责

| 组件 | 作用 |
| --- | --- |
| `HtmlClipboardItem` | 一条记录：纯文本 `text`、HTML 源 `html`、原始 MIME 映射 `formats`、时间戳、备注、标签、哈希 |
| `HtmlItemDelegate` | `QStyledItemDelegate` 子类；负责尺寸缓存、`QTextDocument` 排版、行号、折叠遮罩、链接命中 |
| `HtmlClipboardList` | `QListWidget` 子类；负责记录增删改查、过滤、去重、固定、标签、右键菜单与快捷键 |
| `HtmlClipboardBadges` | 独立的小工具模块，只负责右侧标签与固定矩形的**布局 + 绘制**，方便复用与测试 |

---

## 构建依赖

- C++17 或更高
- Qt6（`Core`、`Gui`、`Widgets` 模块）
- Meson `>= 1.3.0`
- Ninja（Meson 后端）

### Debian / Ubuntu

```bash
sudo apt install -y meson ninja-build qt6-base-dev
```

### Arch Linux

```bash
sudo pacman -S meson ninja qt6-base
```

---

## 构建与运行

```bash
meson setup build
meson compile -C build
./build/cliplist
```
---

## 示例

`Cliplist` 构造时插入了三条示例记录并固定第一条：

| 行 | 标签 | 内容 | 固定 |
| --- | --- | --- | --- |
| 1 | 重要 | `plain fallback` | 是 |
| 2 | 链接 | `https://qt.io`（同时含 `text/uri-list`） | 否 |
| 3 | HTML | `<h3>title</h3><p><a href="https://qt.io">qt.io</a></p>` | 否 |

固定条目会渲染成右侧淡灰色矩形（无文字、无圆角），用户标签位于矩形上方。

---

## 嵌入到其它工程

只需编译 `htmlclipboardlist.cpp`、`htmlclipboardbadges.cpp` 并开启 `moc`（CMake 用 `CMAKE_AUTOMOC`，Meson 用 `qt6.compile_moc`）。

```cpp
#include "htmlclipboardlist.h"

auto *list = new HtmlClipboardList(200, parent); // 最多 200 条
list->setNumberingEnabled(true);                 // 行号从 1 开始
list->setMaxItemHeight(240);                     // 单条最高 240px

connect(list, &HtmlClipboardList::linkClicked, this,
        [](const QUrl &url, int){ QDesktopServices::openUrl(url); });

// 直接喂 QMimeData，即刻成为剪贴板历史面板
connect(qApp->clipboard(), &QClipboard::dataChanged, this, [list]{
    list->appendFromMimeData(qApp->clipboard()->mimeData());
});

// 或手工构造
HtmlClipboardItem rec;
rec.text = "plain fallback";
rec.html = "<h3>title</h3><p><a href=\"https://qt.io\">qt.io</a></p>";
rec.label = QStringLiteral("重要");
list->appendRecord(rec);
```

### 常用 API 速查

```cpp
// 记录管理
appendRecord / insertRecord / appendText / appendFromMimeData / appendUnique
recordAt / formatsAt / moveToTop / removeSelected / removeRow
setMaxRecords / setIgnoreDuplicates

// 显示选项
setRichTextEnabled / setTextWrapping / setNumberingEnabled
setFirstRowNumber / setMaxItemHeight / setItemStyleSheet

// 行内状态
setRowExpanded / isRowExpanded
setRecordPinned / isRecordPinned / toggleRecordPinned / pinnedCount
setRecordLabel / recordLabel

// 过滤与复制
setFilterText / filterText / visibleRecordCount
selectedText / copyRecordToClipboard / renderRecordPreview
```

**信号**：`recordActivated`、`recordExpanded`、`linkClicked`、`recordRemoved`、`recordCountChanged`、`recordPinnedChanged`、`recordLabelChanged`。

---

## 交互一览

| 操作 | 说明 |
| --- | --- |
| 单击超链接 | 触发 `linkClicked(url, row)` |
| 单击折叠遮罩 | 展开该行 |
| 右键菜单 | 复制 / 复制为纯文本 / 展开收起 / 固定条目 / 设置标签 / 删除所选 / 清空历史 |
| `Delete` | 删除选中项 |
| `Escape` | 清除过滤条件 |
| `Enter` / `Return` | 激活当前行并发出 `recordActivated` |
| 双击 | 同 `Enter` |

---

## 许可证

本项目基于 **GNU General Public License v3.0 or later** 发布。详见根目录 `LICENSE` 或 <https://www.gnu.org/licenses/gpl-3.0>。

---

## 致谢

设计灵感来自 [CopyQ](https://github.com/hluk/CopyQ) 的剪贴板列表渲染链路（`clipboardbrowser` / `itemdelegate` / `ItemRender` / `theme` 等），本项目仅抽取其中与「把 HTML 渲染进列表行」相关的部分，去除所有平台与插件依赖后重新组织为可独立复用的组件。
