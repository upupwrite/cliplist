#include "cliplist.h"
#include <QApplication>

int main(int argc, char *argv[]) {
	QApplication a(argc, argv);
	Cliplist w;
	w.show();
	return a.exec();
}
