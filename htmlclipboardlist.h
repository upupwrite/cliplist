// SPDX-License-Identifier: GPL-3.0-or-later
//
// htmlclipboardlist.h
//
// 从 CopyQ 的「剪切板列表渲染」链路中抽取出的独立组件。
//
// CopyQ 里对应关系：
//   src/gui/clipboardbrowser.{h,cpp}   ClipboardBrowser(QListView) 视图配置
//   src/item/itemdelegate.{h,cpp}      ItemDelegate：行尺寸缓存 +
//   sizeHintChanged src/item/itemrender.cpp            renderAsHtml()：纯文本
//   -> HTML 的转义/链接化 src/item/itemwidgetcontent.cpp     用 QTextDocument
//   绘制富文本内容 src/gui/theme.{h,cpp}              由调色板生成
//   QTextDocument 样式表 src/item/clipboardmodel.h          记录容器（本组件用
//   QListWidget 自身承担）
//
// 本文件只依赖 Qt Widgets/Gui/Core，不引用 CopyQ 的 Theme / ItemFactory /
// ItemWidget / ClipboardModel / Platform* / Option / Settings 等任何模块，
// 也不含 .ui 文件与插件机制，可直接丢进任意 Qt 工程（需开启 CMAKE_AUTOMOC）。
//
// ---------------------------------------------------------------------------
// 组成
//   HtmlClipboardItem   一条剪切板记录（纯文本 / HTML / 其它 MIME 格式）
//   HtmlItemDelegate    用 QTextDocument 在每行渲染
//   HTML，带文档缓存、按宽重排、
//                       高度变化通知、正文超链接命中、超长行折叠 + 渐变遮罩
//   HtmlClipboardList   QListWidget 子类：即“可渲染 HTML 的剪切板记录展示组件”
//
// ---------------------------------------------------------------------------
// 用法（Qt 5.9+ / Qt 6，CMake 里 find_package(Qt5 Widgets) 即可）
//
//   auto *list = new HtmlClipboardList(200, this);      // 最多 200 条
//   list->setNumberingEnabled(true);                    // 行号从 1 开始
//   list->setMaxItemHeight(240);                        // 单条最高
//   240px，点底部遮罩展开 list->setIgnoreDuplicates(true); //
//   内容相同则置顶而非新增
//
//   connect(list, &HtmlClipboardList::recordActivated, this,
//           [](int row, const HtmlClipboardItem &rec){ /* 回车/双击回填 */ });
//   connect(list, &HtmlClipboardList::linkClicked, this,
//           [](const QUrl &url, int){ QDesktopServices::openUrl(url); });
//
//   // 方式一：直接喂 QMimeData（在 QClipboard::dataChanged
//   里调用即成历史面板） connect(qApp->clipboard(), &QClipboard::dataChanged,
//   this, [list]{
//       list->appendFromMimeData(qApp->clipboard()->mimeData());
//   });
//   // 方式二：手工构造记录
//   HtmlClipboardItem rec;
//   rec.text = "plain fallback";
//   rec.html = "<h3>title</h3><p><a href=\"https://qt.io\">qt.io</a></p>";
//   list->appendRecord(rec);
// ---------------------------------------------------------------------------
#pragma once

#include <QHash>
#include <QIcon>
#include <QListWidget>
#include <QMetaType>
#include <QPixmap>
#include <QPointer>
#include <QString>
#include <QStyledItemDelegate>
#include <QTextDocument>
#include <QUrl>
#include <QVariantMap>

QT_BEGIN_NAMESPACE
class QAbstractItemView;
class QContextMenuEvent;
class QKeyEvent;
class QMimeData;
class QMouseEvent;
class QResizeEvent;
QT_END_NAMESPACE

/** 存放在 QListWidgetItem::data(role) 里的自定义角色。 */
enum HtmlClipboardRole {
	TextRole = Qt::UserRole + 1, ///< QString 纯文本内容
	HtmlSourceRole,				 ///< QString HTML 源（空则由纯文本转义生成）
	FormatsRole,				 ///< QVariantMap 原始 MIME -> QByteArray
	TimestampRole,				 ///< qint64 毫秒时间戳（0 = 未设置）
	NoteRole,					 ///< QString 备注，渲染在内容下方
	HashRole,					 ///< quint32 内容哈希（去重/置顶）
	ExpandedRole,				 ///< bool 超长内容是否展开
	PinnedRole,					 ///< bool 置顶保护，淘汰超限记录时跳过
};

/**
 * 一条剪切板记录。
 *
 * html 为空时按纯文本渲染：转义 HTML 元字符、保留换行与连续空格、自动识别超链接
 * （等价于 CopyQ ItemRender::renderAsHtml 的纯文本分支）。
 */
struct HtmlClipboardItem {
	QString text;		  ///< 纯文本
	QString html;		  ///< HTML 源（可为空）
	QVariantMap formats;  ///< 其它格式，例如 image/png、text/uri-list
	qint64 timestamp = 0; ///< 记录时间（ms）
	QString note;		  ///< 备注/标签
	quint32 hash = 0;	  ///< 内容哈希，0 = 由 text+html 计算
	QIcon icon;			  ///< 可选类型图标，画在行号右侧

	bool isValid() const { return !text.isEmpty() || !html.isEmpty(); }
};

/**
 * 用 QTextDocument 把 HTML 渲染进列表每一行的 delegate。
 *
 * 沿用 CopyQ ItemDelegate 的做法：
 *  - 每行缓存一份 QTextDocument，避免每帧重新解析 HTML；
 *  - 视图宽度变化时重排文档，并通过 sizeHintChanged 让视图刷新行高；
 *  - 模型行增删移/重置时整表回收缓存，不保留悬垂指针。
 * 另外提供：行号栏、类型图标、超长折叠 + 底部渐变、正文超链接点击。
 */
class HtmlItemDelegate final : public QStyledItemDelegate {
	Q_OBJECT

  public:
	explicit HtmlItemDelegate(QObject *parent = nullptr);
	~HtmlItemDelegate() override;

	/** 记录所属视图并订阅其模型的结构变更。 */
	void setView(QAbstractItemView *view);

	/** 排版宽度（一般为视图视口宽度）；变化会重排所有行并发出尺寸变更通知。 */
	void setTextWidth(int width);
	int textWidth() const { return m_textWidth; }

	/** 单条记录最大像素高度，0 = 不限制。 */
	void setMaxItemHeight(int height);
	int maxItemHeight() const { return m_maxItemHeight; }

	/** 是否在行首绘制行号。 */
	void setNumberingEnabled(bool enabled);
	bool isNumberingEnabled() const { return m_numbering; }

	/** 行号起始值（对应 CopyQ 的 rowIndexFromOne 选项）。 */
	void setFirstRowNumber(int firstNumber);
	int firstRowNumber() const { return m_firstRowNumber; }

	/** 允许渲染 HTML；false 时所有记录一律按纯文本渲染。 */
	void setRichTextEnabled(bool enabled);
	bool isRichTextEnabled() const { return m_richText; }

	/** true 按视图宽度折行；false 用固定宽度排版（长行不折，出现横向滚动条）。
	 */
	void setTextWrapping(bool wrap);
	bool isTextWrapping() const { return m_textWrap; }

	/** 追加在基础样式表之后的自定义 CSS（字体、代码块、表格等）。 */
	void setStyleSheetExtra(const QString &styleSheet);
	QString styleSheetExtra() const { return m_styleSheetExtra; }

	void paint(QPainter *painter, const QStyleOptionViewItem &option,
			   const QModelIndex &index) const override;
	QSize sizeHint(const QStyleOptionViewItem &option,
				   const QModelIndex &index) const override;
	bool editorEvent(QEvent *event, QAbstractItemModel *model,
					 const QStyleOptionViewItem &option,
					 const QModelIndex &index) override;

	/** 把某行渲染为位图（CopyQ renderItemPreview 的等价实现）。 */
	QPixmap renderPreview(const QModelIndex &index, int width,
						  int maxHeight = 0) const;

	/** 丢弃某行缓存并请求重新计算行高。 */
	void invalidateRow(const QModelIndex &index);

	/** 折叠状态下正文在宽度 @a width 下的完整高度（用于判断是否需要遮罩）。 */
	int fullHeightOf(const QModelIndex &index, int width);

  signals:
	/** 某行 sizeHint 需要重新计算（视图应做 doItemsLayout）。 */
	void sizeHintChanged(const QModelIndex &index);
	/** 所有行的排版参数变化，需要整表重算。 */
	void allSizesChanged();
	/** 正文中的超链接被点击。 */
	void linkActivated(const QUrl &url, const QModelIndex &index);

  private:
	struct Cache {
		QTextDocument *document = nullptr;		   ///< 常规配色
		QTextDocument *selectedDocument = nullptr; ///< 选中配色，惰性构建
		QSize size;								   ///< 含边距的整行尺寸
		QString html;							   ///< 生成 document 用的 HTML
		QString note;
		int width = -1;		///< 文档可用宽度
		int maxHeight = -1; ///< 生效的最大高度
		int fullHeight = 0; ///< 不折叠时的正文高度
		bool expanded = false;
		QFont font;
	};

	/** 取得（必要时重建）某行缓存。 */
	Cache *prepareCache(int row, const QStyleOptionViewItem &option,
						int width) const;
	/** 重建文档与尺寸。 */
	void rebuild(Cache &cache, const QStyleOptionViewItem &option, int width,
				 const QString &html, const QString &note, bool expanded,
				 const QFont &font) const;
	/** 由调色板生成 QTextDocument 基础样式表（对应 CopyQ Theme）。 */
	QString buildStyleSheet(const QStyleOptionViewItem &option,
							bool selected) const;
	/** 由记录内容组装出待渲染的 HTML。 */
	QString composeHtml(const QListWidgetItem *item) const;
	/** 去掉行号栏/边距后的正文宽度。 */
	int contentWidth(int viewWidth) const;
	/** 行号栏宽度。 */
	int gutterWidth(const QFontMetrics &fm) const;
	/** 命中正文中的链接（无链接返回空 URL）。 */
	QUrl urlAt(const Cache &cache, const QStyleOptionViewItem &option,
			   const QPointF &pos, bool selected) const;
	void purgeAll();
	void dropRow(int row);
	void relayoutAll();

  private:
	QPointer<QListView> m_view;
	int m_textWidth = 0;
	int m_maxItemHeight = 0;
	int m_firstRowNumber = 1;
	int m_margin = 4;
	int m_spacing = 6;
	bool m_numbering = true;
	bool m_richText = true;
	bool m_textWrap = true;
	QString m_styleSheetExtra;
	mutable QHash<int, Cache> m_cache; ///< key = row；结构变更即整表回收
};

/**
 * 可渲染 HTML 的剪切板记录展示组件（QListWidget 子类）。
 *
 * 保留 QListWidget 的既有用法：item(row)/currentItem()/clear()/selectedItems()
 * 等 照常可用，内容通过 HtmlClipboardItem 或对 item 调用 setData(role)
 * 写入即可富渲染。
 */
class HtmlClipboardList final : public QListWidget {
	Q_OBJECT

  public:
	explicit HtmlClipboardList(QWidget *parent = nullptr);
	explicit HtmlClipboardList(int maxRecords, QWidget *parent = nullptr);

	// ---- 记录管理 -------------------------------------------------------
	/** 新记录插到队首（剪切板历史的常规行为），返回所在行。 */
	int appendRecord(const HtmlClipboardItem &record);
	/** 在 @a row 插入记录（负数或越界 = 插到队首）。 */
	int insertRecord(int row, const HtmlClipboardItem &record);
	/** 便捷接口：由纯文本/HTML 追加一条记录。 */
	int appendText(const QString &text, const QString &html = QString());
	/** 从 QMimeData 追加：自动提取 text/html、text/plain，其余格式原样保留。 */
	int appendFromMimeData(const QMimeData *mimeData);
	/** 追加并去重：已存在同哈希记录则移到队首。 */
	int appendUnique(const HtmlClipboardItem &record);
	/** 把某行移到队首。 */
	bool moveToTop(int row);
	/** 读取某行记录。 */
	HtmlClipboardItem recordAt(int row) const;
	/** 某行的原始 MIME 映射。 */
	QVariantMap formatsAt(int row) const;
	/** 删除选中项，返回删除条数。 */
	int removeSelected();
	/** 删除一行。 */
	void removeRow(int row);
	/** 记录条数。 */
	int recordCount() const { return count(); }
	/** 选中记录条数。 */
	int selectedCount() const { return selectedItems().size(); }
	/** 记录上限（0 = 不限）。 */
	void setMaxRecords(int maxRecords);
	int maxRecords() const { return m_maxRecords; }

	// ---- 显示选项（转发给 delegate） -----------------------------------
	void setRichTextEnabled(bool enabled);
	bool isRichTextEnabled() const;
	void setTextWrapping(bool wrap);
	bool isTextWrapping() const;
	void setNumberingEnabled(bool enabled);
	bool isNumberingEnabled() const;
	void setFirstRowNumber(int firstNumber);
	void setMaxItemHeight(int height);
	int maxItemHeight() const;
	/** 追加到富文本基础样式表之后的自定义 CSS。 */
	void setItemStyleSheet(const QString &styleSheet);
	/** 内容相同的记录是否只置顶不新增。 */
	void setIgnoreDuplicates(bool ignore);
	bool isIgnoringDuplicates() const { return m_ignoreDuplicates; }

	// ---- 过滤（CopyQ filterItems 的轻量等价实现：行隐藏） ----------------
	void setFilterText(const QString &text);
	QString filterText() const { return m_filterText; }
	int visibleRecordCount() const;

	// ---- 其它 -----------------------------------------------------------
	/** 选中记录文本的拼接（CopyQ selectedText 的等价实现）。 */
	QString selectedText() const;
	/** 把某行内容按原始 MIME 写回系统剪贴板。 */
	void copyRecordToClipboard(int row);
	/** 渲染某行预览图。 */
	QPixmap renderRecordPreview(int row, int maxWidth, int maxHeight) const;
	/** 展开/收起某行（超长记录）。 */
	void setRowExpanded(int row, bool expanded);
	bool isRowExpanded(int row) const;
	/** 内部 delegate，需要更深定制时直接取用。 */
	HtmlItemDelegate *itemRenderer() const { return m_renderer; }

  signals:
	/** 回车/双击一条记录（对应 CopyQ 的“选中并回填到输入框”）。 */
	void recordActivated(int row, const HtmlClipboardItem &record);
	/** 超长记录被展开或收起。 */
	void recordExpanded(int row, bool expanded);
	/** 正文超链接被点击。 */
	void linkClicked(const QUrl &url, int row);
	/** 记录被删除。 */
	void recordRemoved(int rowsRemoved);
	/** 记录数变化。 */
	void recordCountChanged(int count);

  protected:
	void resizeEvent(QResizeEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;

  private:
	void init();
	/** 依据当前视口宽度刷新 delegate 的排版宽度。 */
	void updateTextWidth();
	bool matchFilter(const HtmlClipboardItem &record) const;
	/** 淘汰超限记录（跳过置顶保护项）。 */
	void allocateSpaceForNewItems(int newItemCount);
	/** 把记录写入 item 的各 data role。 */
	void fillItem(QListWidgetItem *item, const HtmlClipboardItem &record) const;
	/** 底部遮罩区域（点击可展开）。 */
	bool inFadeStrip(const QPoint &pos) const;

  private:
	HtmlItemDelegate *m_renderer = nullptr;
	int m_maxRecords = 0;
	bool m_ignoreDuplicates = false;
	QString m_filterText;
};

Q_DECLARE_METATYPE(HtmlClipboardItem)
