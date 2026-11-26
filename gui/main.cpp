// puke_gui: the puke desktop application.
//
//   puke_gui [project.puke]

#include <QApplication>

#include "MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("puke");

    pukegui::applyTheme();

    pukegui::MainWindow window;
    window.show();
    const QStringList args = QApplication::arguments();
    if (args.size() > 1) window.openProject(args[1]);
    return QApplication::exec();
}
