// SPDX-License-Identifier: GPL-3.0-or-later
//
// cliplist.cpp
//
#include "cliplist.h"

Cliplist::Cliplist() : QMainWindow() {
	initUi();

	HtmlClipboardItem textRecord;
	textRecord.text = "plain fallback";
	textRecord.label = QStringLiteral("重要");
	textRecord.formats.insert(QStringLiteral("text/plain"),
							  textRecord.text.toUtf8());
	clipboard_list->appendRecord(textRecord); // 行 0

	HtmlClipboardItem linkRecord;
	linkRecord.text = "https://qt.io";
	linkRecord.label = QStringLiteral("链接");
	linkRecord.formats.insert(QStringLiteral("text/plain"),
							  linkRecord.text.toUtf8());
	linkRecord.formats.insert(QStringLiteral("text/uri-list"),
							  linkRecord.text.toUtf8());
	clipboard_list->appendRecord(linkRecord); // 行 0（textRecord 变行 1）

	HtmlClipboardItem htmlRecord;
	htmlRecord.text = "plain fallback";
	htmlRecord.label = QStringLiteral("HTML");
	htmlRecord.html =
		"<h3>title</h3><p><a href=\"https://qt.io\">qt.io</a></p>";
	htmlRecord.formats.insert(QStringLiteral("text/plain"),
							  htmlRecord.text.toUtf8());
	htmlRecord.formats.insert(QStringLiteral("text/html"),
							  htmlRecord.html.toUtf8());
	clipboard_list->appendRecord(htmlRecord); // 行 0（前面依次变行 1、2）

	// 关键修复：新签名是 (row, id, pinned)。
	//   row    = 0 —— 该条目“应占据的行位置”（pinrow）
	//   id     = recordIdAt(0) —— 当前第 0 行（即 htmlRecord）的唯一标识
	//   pinned = true
	// 固定后 htmlRecord 会被 relocate 到第 0 行；此后任何 appendRecord
	// 插入到第 0 行时，htmlRecord 都会被重新顶回第 0 行，新记录落到第 1 行。
	clipboard_list->setRecordPinned(0, clipboard_list->recordIdAt(0), true);

	HtmlClipboardItem Record;
	Record.text = "plain fallback";
	Record.label = QStringLiteral("NEW");
	Record.html = "<h3>title</h3><p><a href=\"https://qt.io\">qt.io</a></p>";
	Record.formats.insert(QStringLiteral("text/plain"), Record.text.toUtf8());
	Record.formats.insert(QStringLiteral("text/html"), Record.html.toUtf8());
	// 即便这里 appendRecord 是“插到队首”，htmlRecord 因被固定在第 0 行，
	// 会由 relocatePinnedRows 重新挪回第 0 行，Record 落到第 1 行。
	clipboard_list->appendRecord(Record);

	clipboard_list->setCurrentRow(0);
}

Cliplist::~Cliplist() = default;

void Cliplist::initUi() {
	clipboard_list = new HtmlClipboardList(this);
	setCentralWidget(clipboard_list);
}
