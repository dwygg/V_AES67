#pragma once
#include <QMainWindow>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QTimer>
#include <QTabWidget>
#include <QGroupBox>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QCloseEvent>
#include <QTableWidget>
#include <QSlider>
#include <QCheckBox>
#include <QSpinBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include "pipe_client.h"

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow();

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    void refreshStatus();
    void onStartStop();
    void onApplySettings();
    void onRefreshRouting();
    void onApplyRouting();
    void onAddDestination();
    void onAddRoute();

private:
    void setupUi();
    void createTrayIcon();
    void updateDisplay(const std::map<std::string, std::string>& status);

    PipeClient m_pipe;
    QTimer*    m_timer;

    // Status tab
    QLabel*    m_engineState;
    QLabel*    m_fps;
    QLabel*    m_txPkts;
    QLabel*    m_rxPkts;
    QLabel*    m_jbDepth;
    QLabel*    m_ptpState;

    // Settings tab
    QLineEdit* m_destAddr;
    QLineEdit* m_destPort;
    QLineEdit* m_sourceAddr;
    QLineEdit* m_sourcePort;

    // Controls
    QPushButton* m_btnStartStop;

    // ---- Routing tab ----
    void buildRoutingFromJson(const QJsonDocument& doc);
    QJsonDocument buildRoutingJson() const;
    QTableWidget* m_destTable;
    QTableWidget* m_routeTable;

    // Tray
    QSystemTrayIcon* m_tray;
    QMenu*   m_trayMenu;
    bool     m_quitOnClose = false;
};
