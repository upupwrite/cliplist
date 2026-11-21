#include "cliplist.h"

Cliplist::Cliplist() : QMainWindow() { initUi(); }

Cliplist::~Cliplist() = default;

void Cliplist::initUi() {
	clipboard_list = new HtmlClipboardList(this);
	setCentralWidget(clipboard_list);
}
