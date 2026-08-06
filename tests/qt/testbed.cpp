/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Minimal Qt widget application used to exercise the native haiku QPA
 * plugin: themed toolbar icons, menus and a text edit for input tests.
 */
#include <QApplication>
#include <QMainWindow>
#include <QToolBar>
#include <QTextEdit>
#include <QIcon>
#include <QMenuBar>
int main(int argc, char** argv) {
	QApplication app(argc, argv);
	QMainWindow win;
	win.setWindowTitle("Qt Native Testbed");
	QToolBar *tb = win.addToolBar("main");
	tb->setIconSize(QSize(24, 24));
	tb->addAction(QIcon::fromTheme("document-new"), "New");
	tb->addAction(QIcon::fromTheme("document-open"), "Open");
	tb->addAction(QIcon::fromTheme("document-save"), "Save");
	tb->addSeparator();
	tb->addAction(QIcon::fromTheme("edit-cut"), "Cut");
	tb->addAction(QIcon::fromTheme("edit-copy"), "Copy");
	tb->addAction(QIcon::fromTheme("edit-paste"), "Paste");
	tb->addSeparator();
	tb->addAction(QIcon::fromTheme("go-home"), "Home");
	tb->addAction(QIcon::fromTheme("folder-open"), "Folder");
	QMenu *m = win.menuBar()->addMenu("File");
	m->addAction(QIcon::fromTheme("document-open"), "Open...");
	m->addAction(QIcon::fromTheme("application-exit"), "Quit");
	QTextEdit *ed = new QTextEdit;
	ed->setPlainText("Stock Debian Qt 6.8 running natively on Vitruvian: real BWindows from app_server, BeOS decorations, native icon theme, working keyboard, clipboard and file dialogs.");
	win.setCentralWidget(ed);
	win.resize(680, 440);
	win.move(300, 180);
	win.show();
	return app.exec();
}
