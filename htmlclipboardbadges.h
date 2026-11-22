// SPDX-License-Identifier: GPL-3.0-or-later
//
// htmlclipboardbadges.h
//
// 列表行右侧 badge 的独立辅助模块：用户标签文本、固定标记的布局与绘制。
// 只依赖 Qt，不引用 HtmlClipboardList / HtmlItemDelegate，便于单独复用与测试。
//
#pragma once

#include <QFontMetrics>
#include <QRect>
#include <QString>

QT_BEGIN_NAMESPACE
class QPainter;
class QStyleOptionViewItem;
QT_END_NAMESPACE

namespace HtmlClipboardBadges {

/** 右侧 badge 的布局结果。 */
struct Layout {
	QRect labelRect;		   ///< 用户标签文本区域（右上方）
	QRect pinRect;			   ///< 固定矩形区域（标签文本下方）
	int reservedWidth = 0;	   ///< 需要从正文宽度中扣除的右侧宽度
	int reservedHeight = 0;	   ///< badge 区域实际占用的高度
	bool showLabel = false;	   ///< 是否有用户标签文本
	bool showPin = false;	   ///< 是否有固定矩形
};

/** 右侧 badge 列的宽度（标签文本与固定矩形取较宽者）。 */
int badgeColumnWidth(const QFontMetrics &fm, bool pinned,
					 const QString &labelText);

/** 保证标签文本与固定矩形都能放下的最小行高。 */
int minimumRowHeight(const QFontMetrics &fm, bool pinned,
					 const QString &labelText, int margin, int verticalSpacing);

/** 计算标签文本在上、固定矩形在下的右侧布局。 */
Layout layout(const QRect &rowRect, const QFontMetrics &fm, bool pinned,
			  const QString &labelText, int margin, int verticalSpacing);

/** 绘制用户标签文本与固定矩形。 */
void paint(QPainter *painter, const QStyleOptionViewItem &option,
		   const Layout &layout, const QString &labelText, bool pinned,
		   bool selected);

} // namespace HtmlClipboardBadges
