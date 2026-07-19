#include <QApplication>
#include "mainwindow.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("AES67 Panel");
    app.setOrganizationName("V_AES67");

    MainWindow window;
    window.show();

    return app.exec();
}
