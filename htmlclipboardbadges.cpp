// SPDX-License-Identifier: GPL-3.0-or-later
//
// htmlclipboardbadges.cpp
//
#include "htmlclipboardbadges.h"
#include "qminmax.h"

#include <QPainter>
#include <QStyleOptionViewItem>

#include <algorithm>

namespace {

int pinBadgeWidth(const QFontMetrics &fm) { return std::max(16, fm.height()); }

int pinBadgeHeight(const QFontMetrics &fm) {
	return std::max(6, fm.height() / 2);
}

} // namespace

namespace HtmlClipboardBadges {

int badgeColumnWidth(const QFontMetrics &fm, bool pinned,
					 const QString &labelText) {
	int width = 0;
	if (!labelText.isEmpty())
		width = fm.horizontalAdvance(labelText);
	if (pinned)
		width = std::max(width, pinBadgeWidth(fm));
	return width;
}

int minimumRowHeight(const QFontMetrics &fm, bool pinned,
					 const QString &labelText, int margin,
					 int verticalSpacing) {
	int height = 2 * margin;
	if (!labelText.isEmpty())
		height += fm.height();
	if (pinned) {
		if (!labelText.isEmpty())
			height += verticalSpacing;
		height += pinBadgeHeight(fm);
	}
	return height;
}

Layout layout(const QRect &rowRect, const QFontMetrics &fm, bool pinned,
			  const QString &labelText, int margin, int verticalSpacing) {
	Layout result;

	const int right = rowRect.left() + rowRect.width() - margin;
	int top = rowRect.top() + margin;

	// 用户标签固定在右上方。
	if (!labelText.isEmpty()) {
		const int width = fm.horizontalAdvance(labelText);
		result.labelRect = QRect(right - width, top, width, fm.height());
		result.showLabel = true;
		top = result.labelRect.bottom() + 1 + verticalSpacing;
	}

	// 固定矩形（无文字、无圆角）紧跟在标签文本下方，同样靠右。
	if (pinned) {
		const int width = pinBadgeWidth(fm) / 1.5;
		const int height = qMax(0, rowRect.height() - margin * 2);
		if (height > 0) {
			result.pinRect = QRect(right - width, top, width, height);
			result.showPin = true;
		}
	}

	result.reservedWidth = badgeColumnWidth(fm, pinned, labelText);
	if (result.showLabel || result.showPin) {
		const int bottom = result.showPin ? result.pinRect.bottom()
										  : result.labelRect.bottom();
		result.reservedHeight = bottom - (rowRect.top() + margin) + 1;
	}
	return result;
}

void paint(QPainter *painter, const QStyleOptionViewItem &option,
		   const Layout &layout, const QString &labelText, bool pinned,
		   bool selected) {
	if (!painter)
		return;

	const QPalette &palette = option.palette;

	if (layout.showLabel) {
		const QColor color = selected
								 ? palette.color(QPalette::HighlightedText)
								 : palette.color(QPalette::Mid).darker(200);
		painter->save();
		painter->setPen(color);
		painter->drawText(layout.labelRect, Qt::AlignRight | Qt::AlignTop,
						  labelText);
		painter->restore();
	}

	if (pinned && layout.showPin) {
		QColor background = palette.color(QPalette::Mid).darker(200);
		background.setAlpha(selected ? 72 : 46);

		painter->save();
		painter->setPen(Qt::NoPen);
		painter->setBrush(background);
		painter->drawRect(layout.pinRect.adjusted(0, 0, -1, -1));
		painter->restore();
	}
}

} // namespace HtmlClipboardBadges
