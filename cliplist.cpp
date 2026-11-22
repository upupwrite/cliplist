#include "cliplist.h"

Cliplist::Cliplist() : QMainWindow() {
	initUi();

	HtmlClipboardItem textRecord;
	textRecord.text = "plain fallback";
	textRecord.label = QStringLiteral("重要");
	textRecord.formats.insert(QStringLiteral("text/plain"),
							  textRecord.text.toUtf8());
	clipboard_list->appendRecord(textRecord);

	HtmlClipboardItem linkRecord;
	linkRecord.text = "https://qt.io";
	linkRecord.label = QStringLiteral("链接");
	linkRecord.formats.insert(QStringLiteral("text/plain"),
							  linkRecord.text.toUtf8());
	linkRecord.formats.insert(QStringLiteral("text/uri-list"),
							  linkRecord.text.toUtf8());
	clipboard_list->appendRecord(linkRecord);

	HtmlClipboardItem htmlRecord;
	htmlRecord.text = "plain fallback";
	htmlRecord.label = QStringLiteral("HTML");
	htmlRecord.html =
		"<h3>title</h3><p><a href=\"https://qt.io\">qt.io</a></p>";
	htmlRecord.formats.insert(QStringLiteral("text/plain"),
							  htmlRecord.text.toUtf8());
	htmlRecord.formats.insert(QStringLiteral("text/html"),
							  htmlRecord.html.toUtf8());
	clipboard_list->appendRecord(htmlRecord);

	// 固定条目：右侧显示无文字、无圆角的淡灰色矩形；用户标签在矩形上方。
	clipboard_list->setRecordPinned(0, true);
	clipboard_list->setCurrentRow(0);
}

Cliplist::~Cliplist() = default;

void Cliplist::initUi() {
	clipboard_list = new HtmlClipboardList(this);
	setCentralWidget(clipboard_list);
}
