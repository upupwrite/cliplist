#include "cliplist.h"

Cliplist::Cliplist() : QMainWindow() {
	initUi();
	HtmlClipboardItem rec;
	rec.text = "plain fallback";
	rec.html = "<h3>title</h3><p><a href=\"https://qt.io\">qt.io</a></p>";
	clipboard_list->appendRecord(rec);
	clipboard_list->appendRecord(rec);
	clipboard_list->appendRecord(rec);
	// clipboard_list->setCurrentRow(0);
}

Cliplist::~Cliplist() = default;

void Cliplist::initUi() {
	clipboard_list = new HtmlClipboardList(this);
	setCentralWidget(clipboard_list);
}
