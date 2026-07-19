#include "mainwindow.h"
#include <QApplication>
#include <QStatusBar>
#include <QStyle>

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("AES67 Virtual Soundcard");
    resize(500, 400);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::refreshStatus);
    m_timer->start(1000);  // 1s refresh

    setupUi();
    createTrayIcon();

    // Initial status fetch
    refreshStatus();
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUi() {
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* mainLayout = new QVBoxLayout(central);

    // ---- Tab Widget ----
    auto* tabs = new QTabWidget(this);
    mainLayout->addWidget(tabs);

    // === Status Tab ===
    auto* statusTab = new QWidget();
    auto* statusLayout = new QVBoxLayout(statusTab);

    // Engine state indicator
    m_engineState = new QLabel("● Connecting...");
    m_engineState->setStyleSheet("font-size: 16px; font-weight: bold; color: #888;");
    statusLayout->addWidget(m_engineState);

    // Metrics grid
    auto* grid = new QGroupBox("Real-time Metrics");
    auto* gridLayout = new QVBoxLayout(grid);

    auto makeRow = [](const QString& label, QLabel*& value) -> QHBoxLayout* {
        auto* row = new QHBoxLayout();
        auto* lb = new QLabel(label);
        lb->setFixedWidth(120);
        value = new QLabel("--");
        value->setStyleSheet("font-size: 14px; font-weight: bold;");
        row->addWidget(lb);
        row->addWidget(value);
        row->addStretch();
        return row;
    };

    gridLayout->addLayout(makeRow("Frames:",       m_fps));
    gridLayout->addLayout(makeRow("TX Packets:",   m_txPkts));
    gridLayout->addLayout(makeRow("RX Packets:",   m_rxPkts));
    gridLayout->addLayout(makeRow("Jitter Depth:", m_jbDepth));
    gridLayout->addLayout(makeRow("PTP State:",    m_ptpState));

    statusLayout->addWidget(grid);
    statusLayout->addStretch();
    tabs->addTab(statusTab, "Status");

    // === Settings Tab ===
    auto* settingsTab = new QWidget();
    auto* settingsLayout = new QVBoxLayout(settingsTab);

    auto* netGroup = new QGroupBox("Network");
    auto* netLayout = new QVBoxLayout(netGroup);

    auto makeSetting = [](const QString& label, QLineEdit*& edit, const QString& def) -> QHBoxLayout* {
        auto* row = new QHBoxLayout();
        row->addWidget(new QLabel(label));
        edit = new QLineEdit(def);
        edit->setFixedWidth(200);
        row->addWidget(edit);
        row->addStretch();
        return row;
    };

    netLayout->addLayout(makeSetting("TX Dest:",     m_destAddr,   "239.69.1.128"));
    netLayout->addLayout(makeSetting("TX Port:",     m_destPort,   "5004"));
    netLayout->addLayout(makeSetting("RX Source:",   m_sourceAddr, "239.69.1.128"));
    netLayout->addLayout(makeSetting("RX Port:",     m_sourcePort, "5004"));

    auto* btnApply = new QPushButton("Apply");
    connect(btnApply, &QPushButton::clicked, this, &MainWindow::onApplySettings);
    netLayout->addWidget(btnApply);

    settingsLayout->addWidget(netGroup);
    settingsLayout->addStretch();
    tabs->addTab(settingsTab, "Settings");

    // ---- Bottom controls ----
    auto* ctrlRow = new QHBoxLayout();
    m_btnStartStop = new QPushButton("Stop");
    connect(m_btnStartStop, &QPushButton::clicked, this, &MainWindow::onStartStop);
    ctrlRow->addWidget(m_btnStartStop);
    ctrlRow->addStretch();
    mainLayout->addLayout(ctrlRow);

    // Status bar
    statusBar()->showMessage("Ready");
}

void MainWindow::createTrayIcon() {
    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(style()->standardIcon(QStyle::SP_MediaVolume));
    m_tray->setToolTip("AES67 Virtual Soundcard");

    m_trayMenu = new QMenu(this);
    m_trayMenu->addAction("Show", this, &QWidget::showNormal);
    m_trayMenu->addAction("Start/Stop", this, &MainWindow::onStartStop);
    m_trayMenu->addSeparator();
    m_trayMenu->addAction("Quit", qApp, &QApplication::quit);

    m_tray->setContextMenu(m_trayMenu);
    m_tray->show();

    connect(m_tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::DoubleClick)
            showNormal();
    });
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!m_quitOnClose) {
        hide();
        m_tray->showMessage("AES67 Panel", "Minimized to tray. Right-click to quit.",
                             QSystemTrayIcon::Information, 2000);
        event->ignore();
    } else {
        event->accept();
    }
}

void MainWindow::refreshStatus() {
    std::string resp = m_pipe.SendCommand("STATUS");
    if (resp.empty()) {
        m_engineState->setText("● Disconnected");
        m_engineState->setStyleSheet("font-size: 16px; font-weight: bold; color: #c00;");
        m_btnStartStop->setText("Start");
        m_fps->setText("--");
        m_txPkts->setText("--");
        m_rxPkts->setText("--");
        m_jbDepth->setText("--");
        m_ptpState->setText("--");
        statusBar()->showMessage("Cannot connect to engine");
        return;
    }

    statusBar()->showMessage("Connected");
    auto s = PipeClient::ParseStatus(resp);
    updateDisplay(s);
}

void MainWindow::updateDisplay(const std::map<std::string, std::string>& s) {
    auto get = [&](const char* k) { auto it = s.find(k); return it != s.end() ? it->second : "--"; };

    QString state = QString::fromStdString(get("state"));
    if (state == "running") {
        m_engineState->setText("● Running");
        m_engineState->setStyleSheet("font-size: 16px; font-weight: bold; color: #0a0;");
        m_btnStartStop->setText("Stop");
    } else if (state == "paused") {
        m_engineState->setText("● Paused");
        m_engineState->setStyleSheet("font-size: 16px; font-weight: bold; color: #c80;");
        m_btnStartStop->setText("Start");
    } else {
        m_engineState->setText("● Stopped");
        m_engineState->setStyleSheet("font-size: 16px; font-weight: bold; color: #888;");
        m_btnStartStop->setText("Start");
    }

    QString fps = QString::fromStdString(get("fps"));
    m_fps->setText(fps);

    m_txPkts->setText(QString::fromStdString(get("tx")));
    m_rxPkts->setText(QString::fromStdString(get("rx")));
    m_jbDepth->setText(QString::fromStdString(get("jb")));

    QString ptp = QString::fromStdString(get("ptp"));
    QString ptpOff = QString::fromStdString(get("ptp_off"));
    QString ptpText;
    if (ptp == "0") ptpText = "FREE_RUN";
    else if (ptp == "1") ptpText = "TRACKING";
    else if (ptp == "2") ptpText = "LOCKED";
    else if (ptp == "3") ptpText = "HOLDOVER";
    else ptpText = ptp;
    m_ptpState->setText(ptpText + "  (" + ptpOff + ")");
}

void MainWindow::onStartStop() {
    // P2 fix: decide from the button's current label (kept in sync every second
    // by updateDisplay) instead of doing an extra STATUS round-trip here. The
    // old code sent STATUS first and returned silently if that reply was empty
    // (e.g. hit the brief window between pipe-instance accepts), so the click
    // "did nothing". Sending START/STOP directly avoids that failure mode.
    const bool running = (m_btnStartStop->text() == "Stop");
    std::string resp = m_pipe.SendCommand(running ? "STOP" : "START");
    if (resp.empty() || resp.rfind("ERR", 0) == 0) {
        statusBar()->showMessage(
            QString("Command failed: %1").arg(QString::fromStdString(resp)), 3000);
        return;
    }
    // Reflect the new state immediately, then let the timer keep it in sync.
    refreshStatus();
}

void MainWindow::onApplySettings() {
    m_pipe.SendCommand("SET dest " + m_destAddr->text().toStdString());
    m_pipe.SendCommand("SET port " + m_destPort->text().toStdString());
    m_pipe.SendCommand("SET source " + m_sourceAddr->text().toStdString());
    // M9-4 (P2): RX Port was collected in the UI (m_sourcePort) but never sent,
    // so the receiver port could not be changed from the panel. Send it now
    // (engine parses "SET sourceport <n>").
    m_pipe.SendCommand("SET sourceport " + m_sourcePort->text().toStdString());
    statusBar()->showMessage("Settings applied", 2000);
}
