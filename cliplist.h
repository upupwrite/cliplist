#pragma once

#include "htmlclipboardlist.h"
#include "qtmetamacros.h"
#include <QMainWindow>

class Cliplist : public QMainWindow {
	Q_OBJECT

  public:
	explicit Cliplist();
	~Cliplist();

  private:
	HtmlClipboardList *clipboard_list;
	void initUi();
};
