// SPDX-License-Identifier: GPL-3.0-or-later
//
// htmlclipboardlist.cpp
//
// HtmlItemDelegate / HtmlClipboardList 的实现。
// 只使用 Qt：QTextDocument 负责 HTML 排版，QListWidget 负责容器与选择。
//
#include "htmlclipboardlist.h"
#include "htmlclipboardbadges.h"

#include <QAbstractItemModel>
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QFontMetrics>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLineEdit>
#include <QLinearGradient>
#include <QMenu>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QTextCursor>
#include <QtAlgorithms>

#include <cmath>

namespace {

/// CopyQ ItemRender::renderAsHtml 的纯文本分支等价实现：
/// 转义 HTML 元字符、把换行变成 <br />、保留制表符与连续空格、URL 变超链接。
QString plainTextToHtml(const QString &text) {
	if (text.isEmpty())
		return QString();

	QString html;
	html.reserve(text.size() + text.size() / 8 + 16);

	for (const QChar c : text) {
		switch (c.unicode()) {
		case u'&':
			html += QLatin1String("&amp;");
			break;
		case u'<':
			html += QLatin1String("&lt;");
			break;
		case u'>':
			html += QLatin1String("&gt;");
			break;
		case u'"':
			html += QLatin1String("&quot;");
			break;
		case u'\n':
			html += QLatin1String("<br />");
			break;
		case u'\t':
			html += QLatin1String("&nbsp;&nbsp;&nbsp;&nbsp;");
			break;
		case u' ':
			html += QLatin1String("&nbsp;");
			break;
		default:
			html += c;
			break;
		}
	}

	// 链接化（对应 CopyQ 里对 URL 的处理，点击行为由 delegate 的 editorEvent
	// 支持）
	static const QRegularExpression urlRe(
		QStringLiteral("(?:https?://|ftp://|www\\.|mailto:)[^\\s<>&\"]+"),
		QRegularExpression::CaseInsensitiveOption);

	QString linked;
	linked.reserve(html.size() + 32);
	int last = 0;
	auto it = urlRe.globalMatch(html);
	while (it.hasNext()) {
		const auto match = it.next();
		linked += html.mid(last, match.capturedStart() - last);
		const QString url = match.captured();
		QString href = url;
		if (href.startsWith(QLatin1String("www.")))
			href.prepend(QLatin1String("http://"));
		linked += QLatin1String("<a href=\"") + href + QLatin1String("\">") +
				  url + QLatin1String("</a>");
		last = match.capturedEnd();
	}
	linked += html.mid(last);

	return QLatin1String("<div>") + linked + QLatin1String("</div>");
}

/// 粗略判断是否是富文本（用于没有显式 HTML 时的自动识别）。
inline bool looksLikeHtml(const QString &text) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
	return Qt::mightBeRichText(text);
#else
	return text.contains(QLatin1Char('<'));
#endif
}

/// 内容哈希（FNV-1a）：跨 Qt 版本稳定，用于去重与置顶定位。
quint32 contentHash(const QString &text, const QString &html) {
	quint32 hash = 2166136261u;
	auto feed = [&hash](const QString &value) {
		for (const QChar c : value) {
			hash ^= static_cast<quint32>(c.unicode());
			hash *= 16777619u;
		}
		hash ^= 0x9e3779b9u;
		hash *= 16777619u;
	};
	feed(text);
	feed(html);
	return hash ? hash : 1u;
}

/// 转义写进属性/内联样式的文本，避免破坏 HTML 结构。
QString escapeText(const QString &text) {
	QString result = text;
	result.replace(QLatin1Char('&'), QLatin1String("&amp;"));
	result.replace(QLatin1Char('<'), QLatin1String("&lt;"));
	result.replace(QLatin1Char('>'), QLatin1String("&gt;"));
	result.replace(QLatin1Char('"'), QLatin1String("&quot;"));
	return result;
}

/// 行号栏宽度：左侧留白 + 当前最大行号宽度 + 右侧间距。
/// 只按实际存在的最大行号计算，行数增加时自动变宽，避免为多余位数留白。
constexpr int kGutterLeftPad = 2;
constexpr int kGutterRightPad = 4;

/// 正文与右侧 badge 列之间的间距，以及用户标签与固定矩形之间的垂直间距。
constexpr int kBadgeGap = 8;
constexpr int kBadgeVerticalSpacing = 0;

int gutterWidthFor(const QFontMetrics &fm, int rows, int firstNumber) {
	if (rows <= 0)
		return 0;
	const int lastNumber = firstNumber + rows - 1;
	return kGutterLeftPad + fm.horizontalAdvance(QString::number(lastNumber)) +
		   kGutterRightPad;
}

/// 选中态占位文本颜色（Qt 5.12 之前没有 PlaceholderText）。
inline QColor placeholderColor(const QPalette &palette) {
#if QT_VERSION >= QT_VERSION_CHECK(5, 12, 0)
	return palette.color(QPalette::PlaceholderText);
#else
	return palette.color(QPalette::Mid);
#endif
}

} // namespace

// ===========================================================================
// HtmlItemDelegate
// ===========================================================================

HtmlItemDelegate::HtmlItemDelegate(QObject *parent)
	: QStyledItemDelegate(parent) {}

HtmlItemDelegate::~HtmlItemDelegate() { purgeAll(); }

void HtmlItemDelegate::setView(QAbstractItemView *view) {
	purgeAll();
	m_view = qobject_cast<QListView *>(view);
	if (!m_view)
		return;

	// 任何结构性变更都会让 row -> Cache 的映射失效，整表回收最安全。
	QAbstractItemModel *model = view->model();
	if (!model)
		return;

	// 先摘掉本 delegate 之前挂在模型上的订阅，避免换模型后重复连接
	disconnect(model, nullptr, this, nullptr);

	connect(model, &QAbstractItemModel::rowsAboutToBeInserted, this,
			[this]() { purgeAll(); });
	connect(model, &QAbstractItemModel::rowsAboutToBeRemoved, this,
			[this]() { purgeAll(); });
	connect(model, &QAbstractItemModel::rowsAboutToBeMoved, this,
			[this]() { purgeAll(); });
	connect(model, &QAbstractItemModel::modelAboutToBeReset, this,
			[this]() { purgeAll(); });
	connect(model, &QAbstractItemModel::layoutAboutToBeChanged, this,
			[this]() { purgeAll(); });
	// 行数跨位数时行号栏宽度会变化，需要让所有行重新测量高度
	connect(model, &QAbstractItemModel::rowsInserted, this, [this]() {
		purgeAll();
		emit allSizesChanged();
	});
	connect(model, &QAbstractItemModel::rowsRemoved, this, [this]() {
		purgeAll();
		emit allSizesChanged();
	});
	connect(model, &QAbstractItemModel::rowsMoved, this, [this]() {
		purgeAll();
		emit allSizesChanged();
	});

	// 内容变化：只回收受影响行，并请求重算行高。
	connect(model, &QAbstractItemModel::dataChanged, this,
			[this](const QModelIndex &topLeft, const QModelIndex &bottomRight) {
				for (int row = topLeft.row(); row <= bottomRight.row(); ++row)
					dropRow(row);
				emit allSizesChanged();
			});
}

void HtmlItemDelegate::setTextWidth(int width) {
	if (m_textWidth == width || width <= 0)
		return;
	m_textWidth = width;
	relayoutAll();
}

void HtmlItemDelegate::setMaxItemHeight(int height) {
	const int value = qMax(0, height);
	if (m_maxItemHeight == value)
		return;
	m_maxItemHeight = value;
	relayoutAll();
}

void HtmlItemDelegate::setNumberingEnabled(bool enabled) {
	if (m_numbering == enabled)
		return;
	m_numbering = enabled;
	relayoutAll();
}

void HtmlItemDelegate::setFirstRowNumber(int firstNumber) {
	const int value = qMax(0, firstNumber);
	if (m_firstRowNumber == value)
		return;
	m_firstRowNumber = value;
	relayoutAll();
}

void HtmlItemDelegate::setRichTextEnabled(bool enabled) {
	if (m_richText == enabled)
		return;
	m_richText = enabled;
	relayoutAll();
}

void HtmlItemDelegate::setTextWrapping(bool wrap) {
	if (m_textWrap == wrap)
		return;
	m_textWrap = wrap;
	// 不折行时退化成固定宽度排版（对应 CopyQ updateItemMaximumSize 里的 2048）
	if (!m_textWrap) {
		m_textWidth = 0;
		setTextWidth(2048);
	} else {
		m_textWidth = 0;
		relayoutAll();
	}
}

void HtmlItemDelegate::setStyleSheetExtra(const QString &styleSheet) {
	if (m_styleSheetExtra == styleSheet)
		return;
	m_styleSheetExtra = styleSheet;
	relayoutAll();
}

void HtmlItemDelegate::purgeAll() {
	for (auto &cache : m_cache) {
		delete cache.document;
		delete cache.selectedDocument;
	}
	m_cache.clear();
}

void HtmlItemDelegate::dropRow(int row) {
	auto it = m_cache.find(row);
	if (it == m_cache.end())
		return;
	delete it->document;
	delete it->selectedDocument;
	m_cache.erase(it);
}

void HtmlItemDelegate::relayoutAll() {
	purgeAll();
	emit allSizesChanged();
}

void HtmlItemDelegate::invalidateRow(const QModelIndex &index) {
	if (!index.isValid())
		return;
	dropRow(index.row());
	emit sizeHintChanged(index);
}

int HtmlItemDelegate::gutterWidth(const QFontMetrics &fm) const {
	const int rows =
		m_view && m_view->model() ? m_view->model()->rowCount() : 0;
	return gutterWidthFor(fm, rows, m_firstRowNumber);
}

QString HtmlItemDelegate::labelForIndex(const QModelIndex &index) const {
	return index.data(LabelRole).toString();
}

int HtmlItemDelegate::contentWidth(const QModelIndex &index,
								   int viewWidth) const {
	const QFont font = m_view ? m_view->font() : qApp->font();
	const QFontMetrics fm(font);

	// 启用行号时，左侧由行号区占位；否则仍使用普通左边距。
	const int left = m_numbering ? gutterWidth(fm) : m_margin;

	// 右侧 badge 列：用户标签与固定矩形共用一列，正文不得进入该区域。
	const QString labelText = labelForIndex(index);
	const bool pinned = index.data(PinnedRole).toBool();
	const int badgeWidth =
		HtmlClipboardBadges::badgeColumnWidth(fm, pinned, labelText);
	const int reserved = badgeWidth > 0 ? badgeWidth + kBadgeGap : 0;

	const int width = viewWidth - left - m_margin - reserved;
	return qMax(24, width);
}

QSize HtmlItemDelegate::sizeHint(const QStyleOptionViewItem &option,
								 const QModelIndex &index) const {
	const int viewWidth = option.rect.width() > 0
							  ? option.rect.width()
							  : (m_textWidth > 0 ? m_textWidth : 400);
	const int width = contentWidth(index, viewWidth);

	if (Cache *cache = prepareCache(index.row(), option, width))
		return cache->size;

	// delegate 用在非 QListWidget 上时的兜底估算
	const QFontMetrics fm(option.font);
	const QString text = index.data(TextRole).toString();
	const int lines = qMax(1, text.count(QLatin1Char('\n')) + 1);
	return QSize(viewWidth,
				 qMax(fm.height(), lines * fm.lineSpacing() + 2 * m_margin));
}

QString HtmlItemDelegate::buildStyleSheet(const QStyleOptionViewItem &option,
										  bool selected) const {
	const QPalette &palette = option.palette;
	const QColor foreground = selected
								  ? palette.color(QPalette::HighlightedText)
								  : palette.color(QPalette::Text);
	const QColor link = palette.color(QPalette::Link);
	const QColor code = selected
							? palette.color(QPalette::Highlight).darker(110)
							: palette.color(QPalette::Base).darker(108);
	const QColor border = palette.color(QPalette::Mid);

	QString style =
		QStringLiteral("* { color:%1; }"
					   "a { color:%2; text-decoration:underline; }"
					   "h1,h2,h3,h4,h5 { color:%1; margin:2px 0; }"
					   "p,div,span,li,td,th { margin:0; }"
					   "code,pre { background:%3; border:1px solid %4; }"
					   "blockquote { color:%1; border-left:3px solid %4; "
					   "padding-left:8px; }"
					   "table { border-collapse:collapse; }"
					   "td,th { border:1px solid %4; }")
			.arg(foreground.name(QColor::HexArgb), link.name(QColor::HexArgb),
				 code.name(QColor::HexArgb), border.name(QColor::HexArgb));

	if (!m_styleSheetExtra.isEmpty()) {
		style += QLatin1Char('\n');
		style += m_styleSheetExtra;
	}
	return style;
}

QString HtmlItemDelegate::composeHtml(const QListWidgetItem *item) const {
	if (!item)
		return QString();

	const QString text = item->data(TextRole).toString();
	const QString source = item->data(HtmlSourceRole).toString();

	QString body;
	if (m_richText && !source.isEmpty())
		body = source;
	else if (m_richText && looksLikeHtml(text) && source.isEmpty())
		body = text;
	else
		body = plainTextToHtml(
			text.isEmpty() ? item->data(Qt::DisplayRole).toString() : text);

	const QString note = item->data(NoteRole).toString();
	if (!note.isEmpty()) {
		body +=
			QStringLiteral("<div style=\"color:%1;\"><small>%2</small></div>")
				.arg(placeholderColor(qApp->palette()).name(QColor::HexArgb),
					 escapeText(note));
	}
	return body;
}

void HtmlItemDelegate::rebuild(Cache &cache, const QStyleOptionViewItem &option,
							   int width, const QString &html,
							   const QString &note, const QString &labelText,
							   bool pinned, bool expanded,
							   const QFont &font) const {
	delete cache.document;
	delete cache.selectedDocument;
	cache.document = nullptr;
	cache.selectedDocument = nullptr;

	auto *document = new QTextDocument;
	document->setDocumentMargin(0);
	document->setDefaultFont(font);
	// 样式表必须在 setHtml 之前设置才会作用于内容
	document->setDefaultStyleSheet(buildStyleSheet(option, false));
	document->setHtml(QStringLiteral("<body>%1</body>").arg(html));
	document->setTextWidth(width);

	cache.document = document;
	cache.html = html;
	cache.note = note;
	cache.labelText = labelText;
	cache.width = width;
	cache.maxHeight = m_maxItemHeight;
	cache.expanded = expanded;
	cache.pinned = pinned;
	cache.font = font;

	cache.fullHeight = static_cast<int>(std::ceil(document->size().height()));

	const QFontMetrics fm(font);
	int height = cache.fullHeight;
	if (!expanded && m_maxItemHeight > 0 && height > m_maxItemHeight)
		height = m_maxItemHeight;
	height = qMax(height, fm.height());

	// 用户标签在上、固定矩形在下；内容太短时行高扩展以容纳 badge。
	const int badgeHeight = HtmlClipboardBadges::minimumRowHeight(
		fm, pinned, labelText, m_margin, kBadgeVerticalSpacing);
	height = qMax(height + 2 * m_margin, badgeHeight);

	int rowWidth = width + m_margin;
	if (m_numbering)
		rowWidth += gutterWidth(fm);
	cache.size = QSize(qMax(rowWidth, 40), height);
}

HtmlItemDelegate::Cache *
HtmlItemDelegate::prepareCache(int row, const QStyleOptionViewItem &option,
							   int width) const {
	auto *list = qobject_cast<QListWidget *>(m_view.data());
	if (!list)
		return nullptr;

	const QListWidgetItem *item = list->item(row);
	if (!item)
		return nullptr;

	const QString html = composeHtml(item);
	const QString note = item->data(NoteRole).toString();
	const bool expanded = item->data(ExpandedRole).toBool();
	const bool pinned = item->data(PinnedRole).toBool();
	const QString labelText = item->data(LabelRole).toString();

	QFont font = option.font;
	const QVariant fontData = item->data(Qt::FontRole);
	if (fontData.canConvert<QFont>())
		font = qvariant_cast<QFont>(fontData);

	auto it = m_cache.find(row);
	if (it == m_cache.end()) {
		Cache cache;
		it = m_cache.insert(row, cache);
	}
	Cache &cache = it.value();

	const bool stale = cache.width != width ||
					   cache.maxHeight != m_maxItemHeight ||
					   cache.expanded != expanded || cache.html != html ||
					   cache.note != note || cache.labelText != labelText ||
					   cache.pinned != pinned || cache.font != font;

	if (stale)
		rebuild(cache, option, width, html, note, labelText, pinned, expanded,
				font);

	return &cache;
}

QUrl HtmlItemDelegate::urlAt(const Cache &cache, const QStyleOptionViewItem &,
							 const QPointF &pos, bool selected) const {
	QTextDocument *document = selected && cache.selectedDocument
								  ? cache.selectedDocument
								  : cache.document;
	if (!document)
		return QUrl();

	// pos 已由调用方换算为 QTextDocument 的坐标系（减去边距/行号栏/图标偏移）
	if (pos.x() < 0 || pos.y() < 0)
		return QUrl();

	const QString href = document->documentLayout()->anchorAt(pos);
	return href.isEmpty() ? QUrl() : QUrl(href);
}

void HtmlItemDelegate::paint(QPainter *painter,
							 const QStyleOptionViewItem &option,
							 const QModelIndex &index) const {
	const bool selected = option.state & QStyle::State_Selected;
	const bool current = option.state & QStyle::State_HasFocus;

	QPalette palette = option.palette;

	painter->save();
	painter->setClipRect(option.rect);

	// 背景：选中高亮 > 交替行 > 底色
	QColor background = palette.color(QPalette::Base);
	if (option.features & QStyleOptionViewItem::Alternate)
		background = background.darker(104);
	if (selected)
		background = palette.color(QPalette::Highlight);
	painter->fillRect(option.rect, background);

	if (selected && (option.state & QStyle::State_HasFocus)) {
		painter->setPen(background.darker(130));
		painter->drawRect(option.rect.adjusted(0, 0, -1, -1));
	} else if (current && !selected) {
		painter->setPen(palette.color(QPalette::Mid));
		painter->drawRect(option.rect.adjusted(0, 0, -1, -1));
	}

	const int width = contentWidth(index, option.rect.width());
	Cache *cache = prepareCache(index.row(), option, width);
	if (!cache || !cache->document) {
		painter->restore();
		return;
	}

	const QFontMetrics fm(cache->font);
	int x = option.rect.left() + m_margin;

	// 行号：从 item 左边缘排版，数字宽度随行号位数自动变化
	if (m_numbering) {
		const int gutter = gutterWidth(fm);
		QColor numberColor = selected ? palette.color(QPalette::HighlightedText)
									  : palette.color(QPalette::Mid);
		if (!selected)
			numberColor = numberColor.darker(120);
		painter->setPen(numberColor);

		// 右侧留出 kGutterRightPad，让行号和内容之间保持间距
		painter->drawText(QRect(option.rect.left(),
								option.rect.top() + m_margin,
								gutter - kGutterRightPad, fm.height()),
						  Qt::AlignRight | Qt::AlignTop,
						  QString::number(m_firstRowNumber + index.row()));

		x = option.rect.left() + gutter;
	}

	// 选中态文档惰性构建（同一份 HTML，只换配色）
	if (selected && !cache->selectedDocument) {
		auto *selectedDocument = cache->document->clone();
		selectedDocument->setDocumentMargin(0);
		selectedDocument->setDefaultFont(cache->font);
		selectedDocument->setDefaultStyleSheet(buildStyleSheet(option, true));
		selectedDocument->setHtml(
			QStringLiteral("<body>%1</body>").arg(cache->html));
		selectedDocument->setTextWidth(width);
		cache->selectedDocument = selectedDocument;
	}

	QTextDocument *document = selected && cache->selectedDocument
								  ? cache->selectedDocument
								  : cache->document;
	document->setTextWidth(width);

	const int shownHeight = cache->size.height() - 2 * m_margin;
	painter->save();
	painter->translate(x, option.rect.top() + m_margin);
	painter->setClipRect(0, 0, width, shownHeight);
	document->drawContents(painter);

	// 折叠遮罩：底部渐变 + 提示文字
	if (m_maxItemHeight > 0 && !cache->expanded &&
		cache->fullHeight > m_maxItemHeight) {
		const int fadeHeight = qMin(30, shownHeight / 2);
		if (fadeHeight > 2) {
			QLinearGradient gradient(0, shownHeight - fadeHeight, 0,
									 shownHeight);
			gradient.setColorAt(0.0,
								QColor(background.red(), background.green(),
									   background.blue(), 0));
			gradient.setColorAt(1.0, background);
			painter->fillRect(0, shownHeight - fadeHeight, width, fadeHeight,
							  gradient);
		}
		painter->setPen(selected ? palette.color(QPalette::HighlightedText)
								 : palette.color(QPalette::Mid));
		painter->drawText(
			QRect(0, shownHeight - fm.height(), width, fm.height()),
			Qt::AlignRight | Qt::AlignBottom, QStringLiteral("... "));
	}
	painter->restore();

	// 右侧 badge：用户标签在右上，固定矩形在其下方，二者与正文宽度互不重叠。
	const auto badgeLayout = HtmlClipboardBadges::layout(
		option.rect, fm, cache->pinned, cache->labelText, m_margin,
		kBadgeVerticalSpacing);
	HtmlClipboardBadges::paint(painter, option, badgeLayout, cache->labelText,
							   cache->pinned, selected);

	painter->restore();
}

bool HtmlItemDelegate::editorEvent(QEvent *event, QAbstractItemModel *model,
								   const QStyleOptionViewItem &option,
								   const QModelIndex &index) {
	if (event->type() == QEvent::MouseButtonRelease) {
		auto *mouse = static_cast<QMouseEvent *>(event);
		if (mouse->button() == Qt::LeftButton) {
			const int width = contentWidth(index, option.rect.width());
			if (Cache *cache = prepareCache(index.row(), option, width)) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
				const QPointF pos = mouse->position();
#else
				const QPointF pos = mouse->pos();
#endif
				const QFontMetrics fm(cache->font);
				QPointF local = pos - QPointF(option.rect.topLeft());
				local.setY(local.y() - m_margin);

				if (m_numbering)
					local.setX(local.x() - gutterWidth(fm));
				else
					local.setX(local.x() - m_margin);

				const QUrl url = urlAt(*cache, option, local,
									   option.state & QStyle::State_Selected);
				if (!url.isEmpty()) {
					emit linkActivated(url, index);
					return true;
				}
			}
		}
	}
	return QStyledItemDelegate::editorEvent(event, model, option, index);
}

QPixmap HtmlItemDelegate::renderPreview(const QModelIndex &index, int width,
										int maxHeight) const {
	if (!m_view || width <= 0)
		return QPixmap();

	QStyleOptionViewItem option;
	option.initFrom(m_view->viewport());
	option.palette = m_view->palette();
	option.font = m_view->font();
	option.rect = QRect(0, 0, width, 0);

	const QSize size = sizeHint(option, index);
	const int height =
		maxHeight > 0 ? qMin(size.height(), maxHeight) : size.height();
	if (height <= 0)
		return QPixmap();

	option.rect = QRect(0, 0, width, height);

	const qreal ratio = m_view->devicePixelRatioF();
	QPixmap pixmap(static_cast<int>(width * ratio),
				   static_cast<int>(height * ratio));
	pixmap.setDevicePixelRatio(ratio);
	pixmap.fill(option.palette.color(QPalette::Base));

	QPainter painter(&pixmap);
	paint(&painter, option, index);
	return pixmap;
}

int HtmlItemDelegate::fullHeightOf(const QModelIndex &index, int width) {
	if (!m_view || !index.isValid() || width <= 0)
		return 0;

	QStyleOptionViewItem option;
	option.initFrom(m_view->viewport());
	option.palette = m_view->palette();
	option.font = m_view->font();
	option.rect = QRect(0, 0, width, 0);

	Cache *cache =
		prepareCache(index.row(), option, contentWidth(index, width));
	return cache ? cache->fullHeight : 0;
}

// ===========================================================================
// HtmlClipboardList
// ===========================================================================

HtmlClipboardList::HtmlClipboardList(QWidget *parent) : QListWidget(parent) {
	init();
}

HtmlClipboardList::HtmlClipboardList(int maxRecords, QWidget *parent)
	: QListWidget(parent), m_maxRecords(qMax(0, maxRecords)) {
	init();
}

void HtmlClipboardList::init() {
	setObjectName(QStringLiteral("HtmlClipboardList"));

	// 与 CopyQ ClipboardBrowser 构造里一致的视图配置
	setLayoutMode(QListView::SinglePass);
	setBatchSize(1);
	setFrameShape(QFrame::NoFrame);
	setTabKeyNavigation(false);
	setSelectionMode(QAbstractItemView::ExtendedSelection);
	setFlow(QListView::TopToBottom);
	setWrapping(false);
	setEditTriggers(QAbstractItemView::NoEditTriggers);
	setAlternatingRowColors(true);
	setUniformItemSizes(false); // 富文本行高不定，必须关闭
	setResizeMode(QListView::Adjust);
	setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
	setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
	setWordWrap(false); // 折行交给 QTextDocument
	setTextElideMode(Qt::ElideNone);
	setAttribute(Qt::WA_MacShowFocusRect, false);

	m_renderer = new HtmlItemDelegate(this);
	setItemDelegate(m_renderer);
	m_renderer->setView(this);

	connect(m_renderer, &HtmlItemDelegate::sizeHintChanged, this,
			[this](const QModelIndex &) { doItemsLayout(); });
	connect(m_renderer, &HtmlItemDelegate::allSizesChanged, this,
			[this]() { doItemsLayout(); });
	connect(m_renderer, &HtmlItemDelegate::linkActivated, this,
			[this](const QUrl &url, const QModelIndex &index) {
				emit linkClicked(url, index.row());
			});

	connect(this, &QListWidget::activated, this,
			[this](const QModelIndex &index) {
				if (index.isValid())
					emit recordActivated(index.row(), recordAt(index.row()));
			});
	connect(model(), &QAbstractItemModel::rowsInserted, this,
			[this]() { emit recordCountChanged(count()); });
	connect(model(), &QAbstractItemModel::rowsRemoved, this,
			[this]() { emit recordCountChanged(count()); });
	connect(this, &QListWidget::itemSelectionChanged, this,
			[this]() { viewport()->update(); });

	updateTextWidth();
}

void HtmlClipboardList::fillItem(QListWidgetItem *item,
								 const HtmlClipboardItem &record) const {
	item->setData(TextRole, record.text);
	item->setData(HtmlSourceRole, record.html);
	item->setData(FormatsRole, record.formats);
	item->setData(TimestampRole, record.timestamp);
	item->setData(NoteRole, record.note);
	item->setData(HashRole, record.hash
								? record.hash
								: contentHash(record.text, record.html));
	item->setData(ExpandedRole, false);
	item->setData(LabelRole, record.label);
	item->setData(IdRole, record.id);
	item->setData(PinnedRole, record.pin);
	// pinrow 仅在固定时才写入，避免把 0 当成“第 0 行”
	item->setData(PinRowRole, record.pin ? record.pinrow : -1);
	item->setData(Qt::DisplayRole, QVariant()); // 内容一律由 delegate 绘制
	item->setData(Qt::TextAlignmentRole, QVariant());
	item->setToolTip(QString());
}

int HtmlClipboardList::insertRecord(int row, const HtmlClipboardItem &record) {
	if (!record.isValid())
		return -1;

	HtmlClipboardItem rec = record;
	if (rec.hash == 0)
		rec.hash = contentHash(rec.text, rec.html);

	if (rec.id <= 0) {
		while (rowForId(m_nextAutoId) >= 0)
			++m_nextAutoId;
		rec.id = m_nextAutoId++;
	} else if (rec.id >= m_nextAutoId) {
		m_nextAutoId = rec.id + 1;
	}

	if (m_ignoreDuplicates) {
		for (int i = 0; i < count(); ++i) {
			if (item(i)->data(HashRole).toUInt() == rec.hash) {
				moveToTop(i);
				return rowForId(rec.id); // 见下方 moveToTop 说明
			}
		}
	}

	allocateSpaceForNewItems(1);

	auto *item = new QListWidgetItem();
	fillItem(item, rec);
	item->setData(HashRole, rec.hash);

	const int target = (row < 0 || row > count()) ? 0 : row;
	insertItem(target, item);

	if (!m_filterText.isEmpty())
		setRowHidden(target, !matchFilter(rec));

	// 关键：新行插入可能把固定项顶走，立刻对齐。
	relocatePinnedRows();

	updateTextWidth();
	doItemsLayout();

	scrollToItem(item, QAbstractItemView::PositionAtTop);
	// 返回该记录当前实际行（relocate 后可能变化）。
	return rowForId(rec.id);
}

int HtmlClipboardList::appendRecord(const HtmlClipboardItem &record) {
	return insertRecord(0, record);
}

int HtmlClipboardList::appendText(const QString &text, const QString &html) {
	HtmlClipboardItem record;
	record.text = text;
	record.html = html;
	record.timestamp = QDateTime::currentMSecsSinceEpoch();
	record.hash = contentHash(text, html);
	record.formats.insert(QStringLiteral("text/plain"), text.toUtf8());
	if (!html.isEmpty())
		record.formats.insert(QStringLiteral("text/html"), html.toUtf8());
	return appendRecord(record);
}

int HtmlClipboardList::appendFromMimeData(const QMimeData *mimeData) {
	if (!mimeData)
		return -1;

	HtmlClipboardItem record;
	record.timestamp = QDateTime::currentMSecsSinceEpoch();

	const auto formats = mimeData->formats();
	for (const QString &format : formats) {
		const QByteArray data = mimeData->data(format);
		if (data.isEmpty())
			continue;
		record.formats.insert(format, data);
	}

	if (mimeData->hasHtml())
		record.html =
			QString::fromUtf8(mimeData->data(QByteArrayLiteral("text/html")));
	if (record.html.isEmpty() && mimeData->hasText())
		record.html = mimeData->html();
	if (mimeData->hasText())
		record.text = mimeData->text();
	if (record.text.isEmpty() && mimeData->hasUrls())
		record.text = mimeData->urls().value(0).toString();

	// 图片记录：保留原始字节；不再生成缩略图标，标签由用户自行设置。
	for (const QByteArray &imageFormat :
		 {QByteArrayLiteral("image/png"), QByteArrayLiteral("image/jpeg"),
		  QByteArrayLiteral("image/bmp"), QByteArrayLiteral("image/gif")}) {
		const QVariant value =
			record.formats.value(QString::fromLatin1(imageFormat));
		if (value.isNull())
			continue;
		if (record.text.isEmpty()) {
			QPixmap pixmap;
			if (pixmap.loadFromData(value.toByteArray())) {
				record.text = QStringLiteral("[image %1x%2]")
								  .arg(pixmap.width())
								  .arg(pixmap.height());
			} else {
				record.text = QStringLiteral("[图片]");
			}
		}
		break;
	}

	record.hash = contentHash(record.text, record.html);
	return appendRecord(record);
}

int HtmlClipboardList::appendUnique(const HtmlClipboardItem &record) {
	HtmlClipboardItem copy = record;
	if (copy.hash == 0)
		copy.hash = contentHash(copy.text, copy.html);
	for (int i = 0; i < count(); ++i) {
		if (item(i)->data(HashRole).toUInt() == copy.hash) {
			moveToTop(i);
			setCurrentRow(i);
			return i;
		}
	}
	return appendRecord(copy);
}

bool HtmlClipboardList::moveToTop(int row) {
	if (row < 0 || row >= count())
		return false;
	if (row == 0)
		return true;
	// 固定项由 relocatePinnedRows 维护位置，moveToTop 会立刻被它挪回去，
	// 因此直接拒绝，语义更清晰。
	if (isRecordPinned(row))
		return false;

	QListWidgetItem *item = takeItem(row);
	insertItem(0, item);
	relocatePinnedRows();
	doItemsLayout();
	return true;
}
HtmlClipboardItem HtmlClipboardList::recordAt(int row) const {
	HtmlClipboardItem record;
	const QListWidgetItem *item = this->item(row);
	if (!item)
		return record;

	record.text = item->data(TextRole).toString();
	record.html = item->data(HtmlSourceRole).toString();
	record.formats = item->data(FormatsRole).toMap();
	record.timestamp = item->data(TimestampRole).toLongLong();
	record.note = item->data(NoteRole).toString();
	record.label = item->data(LabelRole).toString();
	record.hash = item->data(HashRole).toUInt();
	record.id = item->data(IdRole).toInt();
	record.pin = item->data(PinnedRole).toBool();
	record.pinrow = item->data(PinRowRole).toInt();
	return record;
}

QVariantMap HtmlClipboardList::formatsAt(int row) const {
	const QListWidgetItem *item = this->item(row);
	return item ? item->data(FormatsRole).toMap() : QVariantMap();
}

int HtmlClipboardList::recordIdAt(int row) const {
	const QListWidgetItem *it = this->item(row);
	return it ? it->data(IdRole).toInt() : -1;
}

int HtmlClipboardList::rowForId(int id) const {
	if (id <= 0)
		return -1;
	for (int i = 0; i < count(); ++i) {
		if (item(i)->data(IdRole).toInt() == id)
			return i;
	}
	return -1;
}

int HtmlClipboardList::recordPinRow(int row) const {
	const QListWidgetItem *it = this->item(row);
	if (!it || !it->data(PinnedRole).toBool())
		return -1;
	return it->data(PinRowRole).toInt();
}

void HtmlClipboardList::removeRow(int row) {
	if (row < 0 || row >= count())
		return;
	delete item(row);
	relocatePinnedRows();
	emit recordRemoved(1);
}

int HtmlClipboardList::removeSelected() {
	const auto items = selectedItems();
	const int removed = items.size();
	qDeleteAll(items);
	if (removed > 0) {
		relocatePinnedRows();
		emit recordRemoved(removed);
	}
	return removed;
}

void HtmlClipboardList::setMaxRecords(int maxRecords) {
	m_maxRecords = qMax(0, maxRecords);
	allocateSpaceForNewItems(0);
}

void HtmlClipboardList::allocateSpaceForNewItems(int newItemCount) {
	if (m_maxRecords <= 0)
		return;

	int overflow = count() + newItemCount - m_maxRecords;
	if (overflow <= 0)
		return;

	// 从队尾淘汰，跳过置顶保护项
	for (int row = count() - 1; row >= 0 && overflow > 0; --row) {
		QListWidgetItem *item = this->item(row);
		if (item->data(PinnedRole).toBool())
			continue;
		delete item;
		--overflow;
	}
}

// ---- 显示选项 ------------------------------------------------------------

void HtmlClipboardList::setRichTextEnabled(bool enabled) {
	m_renderer->setRichTextEnabled(enabled);
	doItemsLayout();
}

bool HtmlClipboardList::isRichTextEnabled() const {
	return m_renderer->isRichTextEnabled();
}

void HtmlClipboardList::setTextWrapping(bool wrap) {
	m_renderer->setTextWrapping(wrap);
	updateTextWidth();
	doItemsLayout();
}

bool HtmlClipboardList::isTextWrapping() const {
	return m_renderer->isTextWrapping();
}

void HtmlClipboardList::setNumberingEnabled(bool enabled) {
	m_renderer->setNumberingEnabled(enabled);
	doItemsLayout();
}

bool HtmlClipboardList::isNumberingEnabled() const {
	return m_renderer->isNumberingEnabled();
}

void HtmlClipboardList::setFirstRowNumber(int firstNumber) {
	m_renderer->setFirstRowNumber(firstNumber);
	doItemsLayout();
}

void HtmlClipboardList::setMaxItemHeight(int height) {
	m_renderer->setMaxItemHeight(height);
	doItemsLayout();
}

int HtmlClipboardList::maxItemHeight() const {
	return m_renderer->maxItemHeight();
}

void HtmlClipboardList::setItemStyleSheet(const QString &styleSheet) {
	m_renderer->setStyleSheetExtra(styleSheet);
	doItemsLayout();
}

void HtmlClipboardList::setIgnoreDuplicates(bool ignore) {
	m_ignoreDuplicates = ignore;
}

// ---- 过滤 ----------------------------------------------------------------

bool HtmlClipboardList::matchFilter(const HtmlClipboardItem &record) const {
	if (m_filterText.isEmpty())
		return true;
	return record.text.contains(m_filterText, Qt::CaseInsensitive) ||
		   record.note.contains(m_filterText, Qt::CaseInsensitive);
}

void HtmlClipboardList::setFilterText(const QString &text) {
	m_filterText = text;
	for (int row = 0; row < count(); ++row)
		setRowHidden(row, !matchFilter(recordAt(row)));
	doItemsLayout();
}

int HtmlClipboardList::visibleRecordCount() const {
	int visible = 0;
	for (int row = 0; row < count(); ++row) {
		if (!isRowHidden(row))
			++visible;
	}
	return visible;
}

// ---- 其它 ----------------------------------------------------------------

QString HtmlClipboardList::selectedText() const {
	QStringList texts;
	const auto items = selectedItems();
	for (const QListWidgetItem *item : items) {
		const QString text = item->data(TextRole).toString();
		if (!text.isEmpty())
			texts << text;
	}
	return texts.join(QLatin1Char('\n'));
}

void HtmlClipboardList::copyRecordToClipboard(int row) {
	const HtmlClipboardItem record = recordAt(row);
	auto *mime = new QMimeData;

	bool restored = false;
	for (auto it = record.formats.constBegin(); it != record.formats.constEnd();
		 ++it) {
		const QByteArray data = it.value().toByteArray();
		if (data.isEmpty())
			continue;
		mime->setData(it.key().toLatin1(), data);
		restored = true;
	}
	if (!restored) {
		if (!record.html.isEmpty())
			mime->setHtml(record.html);
		mime->setText(record.text);
	}
	qApp->clipboard()->setMimeData(mime);
}

QPixmap HtmlClipboardList::renderRecordPreview(int row, int maxWidth,
											   int maxHeight) const {
	if (row < 0 || row >= count() || maxWidth <= 0)
		return QPixmap();
	return m_renderer->renderPreview(model()->index(row, 0), maxWidth,
									 maxHeight);
}

void HtmlClipboardList::setRowExpanded(int row, bool expanded) {
	QListWidgetItem *item = this->item(row);
	if (!item || item->data(ExpandedRole).toBool() == expanded)
		return;
	item->setData(ExpandedRole, expanded);
	m_renderer->invalidateRow(model()->index(row, 0));
	doItemsLayout();
	emit recordExpanded(row, expanded);
}

bool HtmlClipboardList::isRowExpanded(int row) const {
	const QListWidgetItem *item = this->item(row);
	return item ? item->data(ExpandedRole).toBool() : false;
}

// ---- 固定 ----------------------------------------------------------------
void HtmlClipboardList::setRecordPinned(int row, int id, bool pinned) {
	// 语义：
	//   row    —— 条目“应占据的行位置”（pinrow），固定后由 relocatePinnedRows()
	//             把它真正挪到该行；其他条目插入/删除时它会被自动纠正回来。
	//   id     —— 记录唯一标识，优先据此定位 item；若无效/找不到则回退按 row
	//             查找（兼容旧式调用）。
	//   pinned —— true 固定，false 取消。
	int targetRow = (id > 0) ? rowForId(id) : -1;
	if (targetRow < 0 && row >= 0 && row < count())
		targetRow = row;
	if (targetRow < 0)
		return;

	QListWidgetItem *target = item(targetRow);
	if (!target)
		return;

	const int actualId = target->data(IdRole).toInt();
	const bool wasPinned = target->data(PinnedRole).toBool();

	if (wasPinned == pinned) {
		// 状态未变；若已固定则允许更新 pinrow（用户可能想把它挪到新行）。
		if (pinned) {
			target->setData(PinRowRole, row);
			relocatePinnedRows();
		}
		return;
	}

	target->setData(PinnedRole, pinned);
	target->setData(PinRowRole, pinned ? QVariant(row) : QVariant(-1));

	m_renderer->invalidateRow(model()->index(targetRow, 0));
	doItemsLayout();

	// 关键：固定后立即把它挪到 row 位置（并把其它固定项按 pinrow 重排）。
	if (pinned)
		relocatePinnedRows();

	emit recordPinnedChanged(rowForId(actualId), actualId, pinned);
}

bool HtmlClipboardList::isRecordPinned(int row) const {
	const QListWidgetItem *it = this->item(row);
	return it ? it->data(PinnedRole).toBool() : false;
}

bool HtmlClipboardList::isRecordPinnedById(int id) const {
	const int row = rowForId(id);
	return row >= 0 && isRecordPinned(row);
}

void HtmlClipboardList::toggleRecordPinned(int row) {
	const int id = recordIdAt(row);
	// 切换时把当前行作为 pinrow（“固定到现在的位置”），
	// 需要“固定到指定位置”的调用方应直接走 setRecordPinned。
	setRecordPinned(row, id, !isRecordPinned(row));
}

void HtmlClipboardList::toggleRecordPinnedById(int id) {
	const int row = rowForId(id);
	if (row >= 0)
		setRecordPinned(row, id, !isRecordPinned(row));
}

int HtmlClipboardList::pinnedCount() const {
	int pinned = 0;
	for (int row = 0; row < count(); ++row) {
		if (isRecordPinned(row))
			++pinned;
	}
	return pinned;
}

// ---- 用户标签 ------------------------------------------------------------

void HtmlClipboardList::setRecordLabel(int row, const QString &label) {
	QListWidgetItem *item = this->item(row);
	if (!item || item->data(LabelRole).toString() == label)
		return;

	item->setData(LabelRole, label);
	m_renderer->invalidateRow(model()->index(row, 0));
	doItemsLayout();
	emit recordLabelChanged(row, label);
}

QString HtmlClipboardList::recordLabel(int row) const {
	const QListWidgetItem *item = this->item(row);
	return item ? item->data(LabelRole).toString() : QString();
}

// ---- 事件 ---------------------------------------------------------------

void HtmlClipboardList::resizeEvent(QResizeEvent *event) {
	QListWidget::resizeEvent(event);
	updateTextWidth();
}

void HtmlClipboardList::updateTextWidth() {
	const int width = viewport()->contentsRect().width();
	if (width <= 0)
		return;
	m_renderer->setTextWidth(m_renderer->isTextWrapping() ? width : 2048);
}

bool HtmlClipboardList::inFadeStrip(const QPoint &pos) const {
	if (m_renderer->maxItemHeight() <= 0)
		return false;

	const QModelIndex hitIndex = indexAt(QPoint(0, pos.y()));
	if (!hitIndex.isValid())
		return false;
	const int row = hitIndex.row();
	if (isRowExpanded(row))
		return false;

	const int viewWidth = viewport()->contentsRect().width();
	const int full =
		m_renderer->fullHeightOf(model()->index(row, 0), viewWidth);
	if (full <= m_renderer->maxItemHeight())
		return false;

	const QRect rect = visualRect(model()->index(row, 0));
	const int fadeHeight = qMin(30, rect.height() / 2);
	return rect.isValid() && pos.y() >= rect.bottom() - fadeHeight &&
		   pos.y() <= rect.bottom();
}

void HtmlClipboardList::mousePressEvent(QMouseEvent *event) {
	if (event->button() == Qt::LeftButton && inFadeStrip(event->pos())) {
		const int row = indexAt(QPoint(0, event->pos().y())).row();
		setRowExpanded(row, true);
		return;
	}
	QListWidget::mousePressEvent(event);
}

void HtmlClipboardList::contextMenuEvent(QContextMenuEvent *event) {
	const int row = currentRow();

	QMenu menu(this);
	QAction *actCopy = menu.addAction(tr("复制"));
	actCopy->setEnabled(row >= 0);
	QAction *actCopyText = menu.addAction(tr("复制为纯文本"));
	actCopyText->setEnabled(row >= 0);
	QAction *actExpand = menu.addAction(tr("展开/收起"));
	actExpand->setEnabled(row >= 0 && m_renderer->maxItemHeight() > 0);
	QAction *actPin = menu.addAction(tr("固定条目"));
	actPin->setCheckable(true);
	actPin->setEnabled(row >= 0);
	if (row >= 0)
		actPin->setChecked(isRecordPinned(row));
	QAction *actLabel = menu.addAction(tr("设置标签..."));
	actLabel->setEnabled(row >= 0);
	menu.addSeparator();
	QAction *actRemove = menu.addAction(tr("删除所选"));
	actRemove->setEnabled(selectedCount() > 0);
	QAction *actClear = menu.addAction(tr("清空历史"));
	actClear->setEnabled(count() > 0);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	QAction *chosen = menu.exec(event->globalPos());
#else
	QAction *chosen = menu.exec(event->globalPos());
#endif
	if (!chosen)
		return;

	if (chosen == actCopy) {
		copyRecordToClipboard(row);
	} else if (chosen == actCopyText) {
		qApp->clipboard()->setText(recordAt(row).text);
	} else if (chosen == actExpand) {
		setRowExpanded(row, !isRowExpanded(row));
	} else if (chosen == actPin) {
		// 通过 id 精确定位，避免期间行重排导致 row 漂移；
		// row 保留为该条目“应固定的位置”（pinrow）。
		setRecordPinned(row, recordIdAt(row), actPin->isChecked());
	} else if (chosen == actLabel) {
		bool ok = false;
		const QString label =
			QInputDialog::getText(this, tr("设置标签"), tr("标签文本："),
								  QLineEdit::Normal, recordLabel(row), &ok);
		if (ok)
			setRecordLabel(row, label);
	} else if (chosen == actRemove) {
		removeSelected();
	} else if (chosen == actClear) {
		const int removed = count();
		clear();
		if (removed > 0)
			emit recordRemoved(removed);
	} else if (chosen == actPin) {
		// row 作为 pinrow（让条目固定在当前行），id 用于精确定位。
		setRecordPinned(row, recordIdAt(row), actPin->isChecked());
	}
}

void HtmlClipboardList::keyPressEvent(QKeyEvent *event) {
	if (event->key() == Qt::Key_Delete) {
		if (!selectedItems().isEmpty()) {
			removeSelected();
			return;
		}
	} else if (event->key() == Qt::Key_Escape && !m_filterText.isEmpty()) {
		setFilterText(QString());
		return;
	} else if (event->key() == Qt::Key_Return ||
			   event->key() == Qt::Key_Enter) {
		const int row = currentRow();
		if (row >= 0) {
			emit recordActivated(row, recordAt(row));
			return;
		}
	}
	QListWidget::keyPressEvent(event);
}
void HtmlClipboardList::relocatePinnedRows() {
	if (m_relocatingPinned)
		return;

	struct PinnedInfo {
		int id;
		int pinrow;
		int currentRow;
	};

	QList<PinnedInfo> infos;
	infos.reserve(count());
	for (int r = 0; r < count(); ++r) {
		const QListWidgetItem *it = item(r);
		if (!it || !it->data(PinnedRole).toBool())
			continue;
		infos.append(
			{it->data(IdRole).toInt(), it->data(PinRowRole).toInt(), r});
	}
	if (infos.isEmpty())
		return;

	// 计算目标位置：按 pinrow 升序，冲突顺延；若实际位置已全部吻合则跳过。
	// 排序：pinrow 升序，同 pinrow 时当前行小者优先（先来先占）。
	std::sort(infos.begin(), infos.end(),
			  [](const PinnedInfo &a, const PinnedInfo &b) {
				  if (a.pinrow != b.pinrow)
					  return a.pinrow < b.pinrow;
				  return a.currentRow < b.currentRow;
			  });

	// 预演目标行位置，判断是否需要 relocate。
	QList<int> targets;
	targets.reserve(infos.size());
	int nextFree = 0;
	bool needsMove = false;
	for (const PinnedInfo &info : infos) {
		int target = qMax(info.pinrow, nextFree);
		target = qMin(target, count() - 1);
		targets.append(target);
		nextFree = target + 1;
		if (target != info.currentRow)
			needsMove = true;
	}
	if (!needsMove)
		return;

	// 按当前行降序 take（从底部往上取，避免索引失效）。
	QList<PinnedInfo> byCurrent = infos;
	std::sort(byCurrent.begin(), byCurrent.end(),
			  [](const PinnedInfo &a, const PinnedInfo &b) {
				  return a.currentRow > b.currentRow;
			  });

	m_relocatingPinned = true;

	QHash<int, QListWidgetItem *> pending;
	pending.reserve(infos.size());
	for (const PinnedInfo &info : byCurrent) {
		QListWidgetItem *it = takeItem(info.currentRow);
		if (it)
			pending.insert(info.id, it);
	}

	// 按 pinrow 升序插回，目标行号已预演，直接落位。
	for (int i = 0; i < infos.size(); ++i) {
		QListWidgetItem *it = pending.take(infos[i].id);
		if (!it)
			continue;
		int target = targets[i];
		target = qMin(target, count());
		insertItem(target, it);
	}

	m_relocatingPinned = false;

	doItemsLayout();
}
