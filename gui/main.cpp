// comp_gui: the comp desktop application.
//
//   comp_gui [project.comp]

#include <QApplication>

#include "MainWindow.h"

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("comp");

    compgui::applyTheme();

    compgui::MainWindow window;
    window.show();
    const QStringList args = QApplication::arguments();
    if (args.size() > 1) window.openProject(args[1]);
    return QApplication::exec();
}
