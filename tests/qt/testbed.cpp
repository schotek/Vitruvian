/*
 * Author: Vláďa Janeček <vlada@janecek.cloud>
 *
 * Qt widget application used to exercise the native haiku QPA plugin.
 *
 * Beyond looking like an ordinary application (themed toolbar icons, menus,
 * a text edit), it is built to be driven and asserted on by run_tests.py:
 *
 *   - the central widget paints four saturated quadrants whose colours occur
 *     nowhere in the theme, so a screenshot can be checked structurally
 *     instead of pixel-by-pixel;
 *   - input events are echoed to stdout as single parseable lines, so
 *     injected evdev events can be traced all the way through input_server
 *     and app_server into Qt;
 *   - single-word commands on stdin drive the parts a screenshot cannot
 *     reach on its own: the file dialog and the clipboard.
 *
 * Commands: dialog | clip-set TEXT | clip-get | title TEXT | quit
 * Every command answers on stdout, so the driver never has to guess whether
 * it was processed.
 *
 * Commands arrive on stdin, or on a fifo given as --fifo PATH. Prefer the
 * fifo: the testbed opens it read-write, so it never sees end-of-file when a
 * writer closes, and a driver can send one command per `printf > path` for as
 * long as the testbed lives. Reading stdin instead means the first writer to
 * close ends the channel for good.
 */
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QIcon>
#include <QMainWindow>
#include <QMenuBar>
#include <QPainter>
#include <QSocketNotifier>
#include <QTextEdit>
#include <QToolBar>
#include <QVBoxLayout>

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

// Saturated primaries plus magenta: none of them appears in the BeOS theme,
// in the icon set or in the desktop background, so finding them in a
// screenshot is unambiguous evidence that our own pixels reached the screen.
static const QColor kQuadrant[4] = {
	QColor(255, 0, 0), QColor(0, 255, 0),
	QColor(0, 0, 255), QColor(255, 0, 255)
};


class PatternWidget : public QWidget {
public:
	explicit PatternWidget(QWidget* parent = nullptr) : QWidget(parent)
	{
		setMinimumSize(240, 160);
	}

protected:
	void paintEvent(QPaintEvent*) override
	{
		QPainter p(this);
		const int w = width() / 2;
		const int h = height() / 2;
		p.fillRect(0, 0, w, h, kQuadrant[0]);
		p.fillRect(w, 0, width() - w, h, kQuadrant[1]);
		p.fillRect(0, h, w, height() - h, kQuadrant[2]);
		p.fillRect(w, h, width() - w, height() - h, kQuadrant[3]);
	}
};


// Reports what Qt received, not what was sent — the point is to prove the
// event survived the whole input_server -> app_server -> QPA chain.
class EventLogger : public QObject {
public:
	bool eventFilter(QObject* obj, QEvent* ev) override
	{
		switch (ev->type()) {
			case QEvent::KeyPress: {
				auto* k = static_cast<QKeyEvent*>(ev);
				printf("EV key press key=0x%x mods=0x%x text=%s\n",
					k->key(), (unsigned)k->modifiers(),
					k->text().toUtf8().constData());
				fflush(stdout);
				break;
			}
			case QEvent::MouseButtonPress: {
				auto* m = static_cast<QMouseEvent*>(ev);
				printf("EV mouse press x=%d y=%d button=%d\n",
					(int)m->position().x(), (int)m->position().y(),
					(int)m->button());
				fflush(stdout);
				break;
			}
			default:
				break;
		}
		return QObject::eventFilter(obj, ev);
	}
};


class Testbed : public QMainWindow {
public:
	explicit Testbed(const QString& fifo)
	{
		setWindowTitle("Qt Native Testbed");

		QToolBar* tb = addToolBar("main");
		tb->setIconSize(QSize(24, 24));
		tb->addAction(QIcon::fromTheme("document-new"), "New");
		tb->addAction(QIcon::fromTheme("document-open"), "Open");
		tb->addAction(QIcon::fromTheme("document-save"), "Save");
		tb->addSeparator();
		tb->addAction(QIcon::fromTheme("edit-cut"), "Cut");
		tb->addAction(QIcon::fromTheme("edit-copy"), "Copy");
		tb->addAction(QIcon::fromTheme("edit-paste"), "Paste");

		QMenu* m = menuBar()->addMenu("File");
		m->addAction(QIcon::fromTheme("document-open"), "Open...");
		m->addAction(QIcon::fromTheme("application-exit"), "Quit");

		QWidget* central = new QWidget;
		QVBoxLayout* box = new QVBoxLayout(central);
		box->setContentsMargins(0, 0, 0, 0);
		box->setSpacing(0);
		fPattern = new PatternWidget;
		fEdit = new QTextEdit;
		fEdit->setPlainText("Qt on Vitruvian: real BWindows from app_server, "
			"BeOS decorations, native icon theme, keyboard, clipboard "
			"and file dialogs.");
		box->addWidget(fPattern, 1);
		box->addWidget(fEdit, 1);
		setCentralWidget(central);

		resize(680, 440);
		move(300, 180);

		// A QSocketNotifier keeps the command channel inside the event loop:
		// a blocking read on another thread would race with everything Qt
		// does on the main one.
		fFd = STDIN_FILENO;
		if (!fifo.isEmpty()) {
			// O_RDWR, not O_RDONLY: this end then counts as a writer too,
			// so closing the driver's end never delivers end-of-file and
			// the channel survives any number of one-shot writes.
			int fd = ::open(fifo.toUtf8().constData(), O_RDWR);
			if (fd >= 0)
				fFd = fd;
			else
				printf("ERR cannot open fifo %s\n", fifo.toUtf8().constData());
		}
		fNotifier = new QSocketNotifier(fFd, QSocketNotifier::Read, this);
		connect(fNotifier, &QSocketNotifier::activated, this, &Testbed::onCommand);

		printf("READY\n");
		fflush(stdout);
	}

private:
	void onCommand()
	{
		char buf[1024];
		ssize_t n = ::read(fFd, buf, sizeof(buf) - 1);
		if (n <= 0) {
			// Only reachable on the stdin path; the fifo never reports EOF.
			fNotifier->setEnabled(false);
			return;
		}
		buf[n] = '\0';
		QString line = QString::fromUtf8(buf).trimmed();
		QString cmd = line.section(' ', 0, 0);
		QString arg = line.section(' ', 1);

		if (cmd == "dialog") {
			// open() rather than exec(): a modal exec would stop this
			// handler from ever answering, and the driver would have no
			// way to tell "dialog is up" from "testbed is wedged".
			QFileDialog* d = new QFileDialog(this, "Open File", "/boot");
			d->setAttribute(Qt::WA_DeleteOnClose);
			d->open();
			reply("DIALOG opened");
		} else if (cmd == "clip-set") {
			QGuiApplication::clipboard()->setText(arg);
			reply("CLIP set " + arg);
		} else if (cmd == "clip-get") {
			reply("CLIP get " + QGuiApplication::clipboard()->text());
		} else if (cmd == "title") {
			setWindowTitle(arg);
			reply("TITLE " + arg);
		} else if (cmd == "quit") {
			reply("BYE");
			QCoreApplication::quit();
		} else if (!cmd.isEmpty()) {
			reply("ERR unknown command " + cmd);
		}
	}

	void reply(const QString& s)
	{
		printf("%s\n", s.toUtf8().constData());
		fflush(stdout);
	}

	PatternWidget*	fPattern;
	QTextEdit*		fEdit;
	QSocketNotifier* fNotifier;
	int				fFd;
};


int
main(int argc, char** argv)
{
	QApplication app(argc, argv);
	QString fifo;
	for (int i = 1; i + 1 < argc; i++) {
		if (QString::fromUtf8(argv[i]) == "--fifo")
			fifo = QString::fromUtf8(argv[i + 1]);
	}
	EventLogger logger;
	app.installEventFilter(&logger);
	Testbed win(fifo);
	win.show();
	return app.exec();
}