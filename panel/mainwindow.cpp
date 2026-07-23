#include "mainwindow.h"
#include <QApplication>
#include <QStatusBar>
#include <QStyle>
#include <QHeaderView>
#include <QScrollArea>
#include <QFrame>

// Helper: configure table header (no stretch-last — we set column widths explicitly)
static void setupTableHeader(QTableWidget* tbl, const QStringList& headers) {
    tbl->setColumnCount(headers.size());
    tbl->setHorizontalHeaderLabels(headers);
    tbl->horizontalHeader()->setStretchLastSection(false);
    tbl->setSelectionBehavior(QAbstractItemView::SelectRows);
    tbl->setAlternatingRowColors(true);
    tbl->verticalHeader()->setVisible(false);
    tbl->setMinimumHeight(100);
}

// Wrap a child widget in a container with centered layout.
// Returns the container (to be passed to setCellWidget).
static QWidget* centerCellWidget(QWidget* child) {
    auto* w = new QWidget();
    auto* lay = new QHBoxLayout(w);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setAlignment(Qt::AlignCenter);
    lay->addWidget(child);
    return w;
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    setWindowTitle("AES67 Virtual Soundcard");
    setMinimumSize(720, 520);
    resize(920, 680);

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &MainWindow::refreshStatus);
    m_timer->start(1000);  // 1s refresh

    setupUi();
    createTrayIcon();

    // Initial status fetch
    refreshStatus();
    // Initial routing load (deferred so engine's pipe is ready)
    QTimer::singleShot(500, this, &MainWindow::onRefreshRouting);
}

MainWindow::~MainWindow() = default;

void MainWindow::setupUi() {
    auto* central = new QWidget(this);
    setCentralWidget(central);
    auto* mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(14, 12, 14, 10);
    mainLayout->setSpacing(10);

    // ---- Tab Widget ----
    auto* tabs = new QTabWidget(this);
    tabs->setStyleSheet(
        "QTabWidget::pane { border: 1px solid #ccc; border-radius: 4px; padding: 8px; }"
        "QTabBar::tab { padding: 6px 18px; }");
    mainLayout->addWidget(tabs, 1);  // stretch=1: tabs fill available space

    // ===================================================================
    //  STATUS TAB
    // ===================================================================
    auto* statusTab = new QWidget();
    auto* statusLayout = new QVBoxLayout(statusTab);
    statusLayout->setSpacing(12);
    statusLayout->setContentsMargins(8, 8, 8, 8);

    // Engine state indicator — big and prominent
    m_engineState = new QLabel("● Connecting...");
    m_engineState->setStyleSheet(
        "font-size: 20px; font-weight: bold; color: #888; padding: 8px 0;");
    m_engineState->setAlignment(Qt::AlignCenter);
    statusLayout->addWidget(m_engineState);

    // Separator
    auto* sep1 = new QFrame();
    sep1->setFrameShape(QFrame::HLine);
    sep1->setFrameShadow(QFrame::Sunken);
    statusLayout->addWidget(sep1);

    // Metrics grid — 2-column layout, readable
    auto* grid = new QGroupBox("Real-time Metrics");
    grid->setStyleSheet("QGroupBox { font-weight: bold; padding-top: 14px; }");
    auto* gridLayout = new QGridLayout(grid);
    gridLayout->setSpacing(8);
    gridLayout->setContentsMargins(16, 18, 16, 12);

    auto makeMetric = [](const QString& label, QLabel*& value) -> QWidget* {
        auto* w = new QWidget();
        auto* row = new QHBoxLayout(w);
        row->setContentsMargins(0, 0, 0, 0);
        auto* lb = new QLabel(label + ":");
        lb->setFixedWidth(110);
        lb->setStyleSheet("font-size: 13px; color: #555;");
        value = new QLabel("--");
        value->setStyleSheet("font-size: 14px; font-weight: bold;");
        row->addWidget(lb);
        row->addWidget(value);
        row->addStretch();
        return w;
    };

    gridLayout->addWidget(makeMetric("Frames",        m_fps),      0, 0);
    gridLayout->addWidget(makeMetric("TX Packets",    m_txPkts),   0, 1);
    gridLayout->addWidget(makeMetric("RX Packets",    m_rxPkts),   1, 0);
    gridLayout->addWidget(makeMetric("Jitter Depth",  m_jbDepth),  1, 1);
    gridLayout->addWidget(makeMetric("PTP State",     m_ptpState), 2, 0);
    statusLayout->addWidget(grid);
    statusLayout->addStretch();
    tabs->addTab(statusTab, "Status");

    // ===================================================================
    //  SETTINGS TAB
    // ===================================================================
    auto* settingsTab = new QWidget();
    auto* settingsOuter = new QVBoxLayout(settingsTab);
    settingsOuter->setContentsMargins(8, 8, 8, 8);
    settingsOuter->setSpacing(14);

    // TX group
    auto* txGroup = new QGroupBox("Transmit (TX)");
    txGroup->setStyleSheet("QGroupBox { font-weight: bold; padding-top: 14px; }");
    auto* txForm = new QGridLayout(txGroup);
    txForm->setSpacing(8);
    txForm->setContentsMargins(16, 18, 16, 12);

    txForm->addWidget(new QLabel("Destination Address:"), 0, 0);
    m_destAddr = new QLineEdit("239.69.1.128");
    m_destAddr->setMinimumWidth(200);
    txForm->addWidget(m_destAddr, 0, 1);

    txForm->addWidget(new QLabel("Destination Port:"), 1, 0);
    m_destPort = new QLineEdit("5004");
    m_destPort->setMaximumWidth(100);
    txForm->addWidget(m_destPort, 1, 1);

    settingsOuter->addWidget(txGroup);

    // RX group
    auto* rxGroup = new QGroupBox("Receive (RX)");
    rxGroup->setStyleSheet("QGroupBox { font-weight: bold; padding-top: 14px; }");
    auto* rxForm = new QGridLayout(rxGroup);
    rxForm->setSpacing(8);
    rxForm->setContentsMargins(16, 18, 16, 12);

    rxForm->addWidget(new QLabel("Source Address:"), 0, 0);
    m_sourceAddr = new QLineEdit("239.69.1.128");
    m_sourceAddr->setMinimumWidth(200);
    rxForm->addWidget(m_sourceAddr, 0, 1);

    rxForm->addWidget(new QLabel("Source Port:"), 1, 0);
    m_sourcePort = new QLineEdit("5004");
    m_sourcePort->setMaximumWidth(100);
    rxForm->addWidget(m_sourcePort, 1, 1);

    settingsOuter->addWidget(rxGroup);

    // Apply button — prominent
    auto* btnApply = new QPushButton("Apply Network Settings");
    btnApply->setMinimumHeight(36);
    btnApply->setStyleSheet("QPushButton { font-weight: bold; font-size: 13px; }");
    connect(btnApply, &QPushButton::clicked, this, &MainWindow::onApplySettings);
    settingsOuter->addWidget(btnApply);

    settingsOuter->addStretch();
    tabs->addTab(settingsTab, "Settings");

    // ===================================================================
    //  ROUTING TAB  (P6)
    // ===================================================================
    auto* routingTab = new QWidget();
    auto* routingLayout = new QVBoxLayout(routingTab);
    routingLayout->setContentsMargins(8, 8, 8, 8);
    routingLayout->setSpacing(12);

    // ---- Destinations ----
    auto* destGroup = new QGroupBox("Destinations — AES67 Streams");
    destGroup->setStyleSheet("QGroupBox { font-weight: bold; padding-top: 14px; }");
    auto* destVLayout = new QVBoxLayout(destGroup);
    destVLayout->setSpacing(6);
    destVLayout->setContentsMargins(12, 18, 12, 10);

    m_destTable = new QTableWidget(0, 4);
    setupTableHeader(m_destTable, {"Name", "Address", "Port", ""});
    m_destTable->setColumnWidth(0, 170);
    m_destTable->setColumnWidth(1, 230);
    m_destTable->setColumnWidth(2, 100);
    m_destTable->setColumnWidth(3, 44);
    m_destTable->setMaximumHeight(160);
    destVLayout->addWidget(m_destTable);

    auto* addDestBtn = new QPushButton("+ Add Destination");
    addDestBtn->setMinimumHeight(30);
    connect(addDestBtn, &QPushButton::clicked, this, &MainWindow::onAddDestination);
    destVLayout->addWidget(addDestBtn);
    routingLayout->addWidget(destGroup);
    // P6: seed with two default destinations so the table isn't empty on first open
    onAddDestination();
    onAddDestination();

    // ---- Routes ----
    auto* routeGroup = new QGroupBox("Routes — Source Channel → Destination Stream");
    routeGroup->setStyleSheet("QGroupBox { font-weight: bold; padding-top: 14px; }");
    auto* routeVLayout = new QVBoxLayout(routeGroup);
    routeVLayout->setSpacing(6);
    routeVLayout->setContentsMargins(12, 18, 12, 10);

    m_routeTable = new QTableWidget(0, 5);
    setupTableHeader(m_routeTable, {"Src Ch", "Dest", "Gain", "Mute", ""});
    m_routeTable->setColumnWidth(0, 90);
    m_routeTable->setColumnWidth(1, 90);
    m_routeTable->setColumnWidth(2, 420);  // wide — gain slider + label need room
    m_routeTable->setColumnWidth(3, 80);
    m_routeTable->setColumnWidth(4, 44);
    m_routeTable->setMaximumHeight(240);
    m_routeTable->verticalHeader()->setDefaultSectionSize(38);
    routeVLayout->addWidget(m_routeTable);

    auto* addRouteBtn = new QPushButton("+ Add Route");
    addRouteBtn->setMinimumHeight(30);
    connect(addRouteBtn, &QPushButton::clicked, this, &MainWindow::onAddRoute);
    routeVLayout->addWidget(addRouteBtn);
    routingLayout->addWidget(routeGroup);
    // P6: seed with two default routes so controls are visible immediately
    onAddRoute();
    onAddRoute();

    // Action buttons
    auto* routingBtnRow = new QHBoxLayout();
    routingBtnRow->setSpacing(10);
    auto* btnRefreshR = new QPushButton("Refresh from Engine");
    btnRefreshR->setMinimumHeight(34);
    connect(btnRefreshR, &QPushButton::clicked, this, &MainWindow::onRefreshRouting);
    auto* btnApplyR = new QPushButton("Apply Routing");
    btnApplyR->setMinimumHeight(34);
    btnApplyR->setStyleSheet(
        "QPushButton { font-weight: bold; font-size: 13px; "
        "background-color: #0078d4; color: white; border-radius: 4px; padding: 6px 24px; }"
        "QPushButton:hover { background-color: #106ebe; }");
    connect(btnApplyR, &QPushButton::clicked, this, &MainWindow::onApplyRouting);
    routingBtnRow->addWidget(btnRefreshR);
    routingBtnRow->addStretch();
    routingBtnRow->addWidget(btnApplyR);
    routingLayout->addLayout(routingBtnRow);

    tabs->addTab(routingTab, "Routing");

    // ===================================================================
    //  BOTTOM CONTROLS
    // ===================================================================
    auto* ctrlRow = new QHBoxLayout();
    ctrlRow->setContentsMargins(0, 4, 0, 0);

    m_btnStartStop = new QPushButton("Stop");
    m_btnStartStop->setMinimumSize(120, 42);
    m_btnStartStop->setStyleSheet(
        "QPushButton { font-size: 15px; font-weight: bold; "
        "border-radius: 4px; padding: 8px 32px; }");
    connect(m_btnStartStop, &QPushButton::clicked, this, &MainWindow::onStartStop);

    ctrlRow->addStretch();
    ctrlRow->addWidget(m_btnStartStop);
    ctrlRow->addStretch();
    mainLayout->addLayout(ctrlRow);

    // Status bar
    statusBar()->setStyleSheet("QStatusBar { font-size: 12px; }");
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
    // P6 fix: use PollStatus with a short timeout so a missing/stuck engine
    // doesn't freeze the UI. The old SendCommand had a 20×50ms retry loop
    // (=1s blocking) that ran every second on the main thread → dragging
    // the window felt impossibly laggy. Now it fails in ≤300ms.
    std::string resp = m_pipe.PollStatus(300);
    if (resp.empty() || resp.rfind("ERR", 0) == 0) {
        m_engineState->setText("● Disconnected");
        m_engineState->setStyleSheet(
            "font-size: 20px; font-weight: bold; color: #c00; padding: 8px 0;");
        m_btnStartStop->setText("Start");
        m_btnStartStop->setStyleSheet(
            "QPushButton { font-size: 15px; font-weight: bold; "
            "border-radius: 4px; padding: 8px 32px; "
            "background-color: #107c10; color: white; }"
            "QPushButton:hover { background-color: #0e6a0e; }");
        m_fps->setText("--");
        m_txPkts->setText("--");
        m_rxPkts->setText("--");
        m_jbDepth->setText("--");
        m_ptpState->setText("--");
        statusBar()->showMessage(resp.empty()
            ? "Cannot connect to engine"
            : QString::fromStdString(resp));
        return;
    }

    statusBar()->showMessage("Engine connected");
    auto s = PipeClient::ParseStatus(resp);
    updateDisplay(s);
}

void MainWindow::updateDisplay(const std::map<std::string, std::string>& s) {
    auto get = [&](const char* k) { auto it = s.find(k); return it != s.end() ? it->second : "--"; };

    // Shared button style templates
    static const char* kBtnGreen =
        "QPushButton { font-size: 15px; font-weight: bold; "
        "border-radius: 4px; padding: 8px 32px; "
        "background-color: #107c10; color: white; }"
        "QPushButton:hover { background-color: #0e6a0e; }";
    static const char* kBtnRed =
        "QPushButton { font-size: 15px; font-weight: bold; "
        "border-radius: 4px; padding: 8px 32px; "
        "background-color: #d13438; color: white; }"
        "QPushButton:hover { background-color: #b52e32; }";
    static const char* kBtnGray =
        "QPushButton { font-size: 15px; font-weight: bold; "
        "border-radius: 4px; padding: 8px 32px; "
        "background-color: #666; color: white; }"
        "QPushButton:hover { background-color: #555; }";

    QString state = QString::fromStdString(get("state"));
    if (state == "running") {
        m_engineState->setText("● Running");
        m_engineState->setStyleSheet(
            "font-size: 20px; font-weight: bold; color: #0a0; padding: 8px 0;");
        m_btnStartStop->setText("Stop");
        m_btnStartStop->setStyleSheet(kBtnRed);
    } else if (state == "paused") {
        m_engineState->setText("● Paused");
        m_engineState->setStyleSheet(
            "font-size: 20px; font-weight: bold; color: #c80; padding: 8px 0;");
        m_btnStartStop->setText("Start");
        m_btnStartStop->setStyleSheet(kBtnGreen);
    } else {
        m_engineState->setText("● Stopped");
        m_engineState->setStyleSheet(
            "font-size: 20px; font-weight: bold; color: #888; padding: 8px 0;");
        m_btnStartStop->setText("Start");
        m_btnStartStop->setStyleSheet(kBtnGreen);
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
    const bool running = (m_btnStartStop->text() == "Stop");
    std::string resp = m_pipe.SendCommand(running ? "STOP" : "START");
    if (resp.empty() || resp.rfind("ERR", 0) == 0) {
        statusBar()->showMessage(
            QString("Command failed: %1").arg(QString::fromStdString(resp)), 3000);
        return;
    }
    refreshStatus();
}

void MainWindow::onApplySettings() {
    m_pipe.SendCommand("SET dest " + m_destAddr->text().toStdString());
    m_pipe.SendCommand("SET port " + m_destPort->text().toStdString());
    m_pipe.SendCommand("SET source " + m_sourceAddr->text().toStdString());
    m_pipe.SendCommand("SET sourceport " + m_sourcePort->text().toStdString());
    statusBar()->showMessage("Settings applied", 2000);
}

// ── P6: Routing tab ──────────────────────────────────────────

void MainWindow::onRefreshRouting() {
    std::string resp = m_pipe.SendCommand("GET_ROUTING");
    if (resp.empty() || resp.rfind("ERR", 0) == 0) {
        statusBar()->showMessage("Routing fetch failed", 2000);
        return;
    }
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(resp), &err);
    if (err.error != QJsonParseError::NoError) {
        statusBar()->showMessage("Routing JSON parse error: " + err.errorString(), 3000);
        return;
    }
    buildRoutingFromJson(doc);
    statusBar()->showMessage("Routing refreshed", 2000);
}

void MainWindow::onApplyRouting() {
    // Visual feedback: briefly disable + show "Applying..." so the user
    // knows the click registered, even if the pipe response takes a moment.
    auto* btn = qobject_cast<QPushButton*>(sender());
    if (btn) {
        btn->setEnabled(false);
        btn->setText("Applying...");
    }

    QJsonDocument doc = buildRoutingJson();
    std::string json = doc.toJson(QJsonDocument::Compact).toStdString();
    std::string resp = m_pipe.SendCommand("SET_ROUTING " + json);
    // Engine returns "OK" (stopped) or "OK reconfiguring" (running, deferred).
    // Both mean the routing was accepted — don't show "failed" for a success.
    bool ok = !resp.empty() && resp.rfind("OK", 0) == 0;
    statusBar()->showMessage(ok ? "Routing applied" : "Routing apply failed", 2000);

    if (btn) {
        btn->setText(ok ? "✓ Applied" : "✗ Failed");
        btn->setStyleSheet(
            ok
            ? "QPushButton { font-weight: bold; font-size: 13px; "
              "background-color: #107c10; color: white; border-radius: 4px; padding: 6px 24px; }"
            : "QPushButton { font-weight: bold; font-size: 13px; "
              "background-color: #d13438; color: white; border-radius: 4px; padding: 6px 24px; }");
        // Revert to normal after 1.5s
        QTimer::singleShot(1500, btn, [btn]() {
            btn->setEnabled(true);
            btn->setText("Apply Routing");
            btn->setStyleSheet(
                "QPushButton { font-weight: bold; font-size: 13px; "
                "background-color: #0078d4; color: white; border-radius: 4px; padding: 6px 24px; }"
                "QPushButton:hover { background-color: #106ebe; }");
        });
    }
}

QJsonDocument MainWindow::buildRoutingJson() const {
    QJsonArray dests;
    for (int r = 0; r < m_destTable->rowCount(); r++) {
        QJsonObject d;
        auto* nameW = m_destTable->cellWidget(r, 0);
        auto* addrW = m_destTable->cellWidget(r, 1);
        auto* portW = m_destTable->cellWidget(r, 2);
        auto* nameEdit = nameW ? nameW->findChild<QLineEdit*>() : nullptr;
        auto* addrEdit = addrW ? addrW->findChild<QLineEdit*>() : nullptr;
        auto* portSpin = portW ? portW->findChild<QSpinBox*>() : nullptr;
        d["name"]    = nameEdit ? nameEdit->text() : "";
        d["address"] = addrEdit ? addrEdit->text() : "";
        d["port"]    = portSpin ? portSpin->value() : 5004;
        dests.append(d);
    }

    QJsonArray routes;
    for (int r = 0; r < m_routeTable->rowCount(); r++) {
        QJsonObject rt;
        QWidget* w0 = m_routeTable->cellWidget(r, 0);
        QWidget* w1 = m_routeTable->cellWidget(r, 1);
        QWidget* gainW = m_routeTable->cellWidget(r, 2);
        QWidget* muteW = m_routeTable->cellWidget(r, 3);
        auto* srcSpin = w0 ? w0->findChild<QSpinBox*>() : nullptr;
        auto* dstSpin = w1 ? w1->findChild<QSpinBox*>() : nullptr;
        auto* slider  = gainW ? gainW->findChild<QSlider*>() : nullptr;
        auto* muteCb  = muteW ? muteW->findChild<QCheckBox*>() : nullptr;
        rt["source"]      = srcSpin ? srcSpin->value() : 0;
        rt["destination"] = dstSpin ? dstSpin->value() : 0;
        rt["gain"]        = slider ? slider->value() / 100.0 : 1.0;
        rt["mute"]        = muteCb ? muteCb->isChecked() : false;
        routes.append(rt);
    }

    QJsonObject root;
    root["destinations"] = dests;
    root["routes"]       = routes;
    return QJsonDocument(root);
}

void MainWindow::buildRoutingFromJson(const QJsonDocument& doc) {
    QJsonObject root = doc.object();
    QJsonArray dests = root["destinations"].toArray();
    QJsonArray routes = root["routes"].toArray();

    // Update destinations in-place
    while (m_destTable->rowCount() < dests.size())
        onAddDestination();
    while (m_destTable->rowCount() > dests.size())
        m_destTable->removeRow(m_destTable->rowCount() - 1);
    for (int i = 0; i < dests.size(); i++) {
        QJsonObject d = dests[i].toObject();
        // cellWidget may be a centerCellWidget wrapper — unwrap via findChild
        QWidget* w0 = m_destTable->cellWidget(i, 0);
        QWidget* w1 = m_destTable->cellWidget(i, 1);
        QWidget* w2 = m_destTable->cellWidget(i, 2);
        auto* name = w0 ? w0->findChild<QLineEdit*>() : nullptr;
        auto* addr = w1 ? w1->findChild<QLineEdit*>() : nullptr;
        auto* port = w2 ? w2->findChild<QSpinBox*>() : nullptr;
        if (name) name->setText(d["name"].toString());
        if (addr) addr->setText(d["address"].toString());
        if (port) port->setValue(d["port"].toInt(5004));
    }

    // Update routes in-place
    while (m_routeTable->rowCount() < routes.size())
        onAddRoute();
    while (m_routeTable->rowCount() > routes.size())
        m_routeTable->removeRow(m_routeTable->rowCount() - 1);
    for (int i = 0; i < routes.size(); i++) {
        QJsonObject r = routes[i].toObject();
        // cellWidget may be a centerCellWidget wrapper — unwrap via findChild
        QWidget* w0 = m_routeTable->cellWidget(i, 0);
        QWidget* w1 = m_routeTable->cellWidget(i, 1);
        QWidget* gainW = m_routeTable->cellWidget(i, 2);
        QWidget* muteW = m_routeTable->cellWidget(i, 3);
        auto* src = w0 ? w0->findChild<QSpinBox*>() : nullptr;
        auto* dst = w1 ? w1->findChild<QSpinBox*>() : nullptr;
        auto* slider  = gainW ? gainW->findChild<QSlider*>() : nullptr;
        auto* label   = gainW ? gainW->findChild<QLabel*>() : nullptr;
        auto* muteCb  = muteW ? muteW->findChild<QCheckBox*>() : nullptr;
        if (src) src->setValue(r["source"].toInt());
        if (dst) dst->setValue(r["destination"].toInt());
        int gainVal = (int)(r["gain"].toDouble(1.0) * 100.0);
        if (slider) slider->setValue(gainVal);
        if (label) label->setText(QString::number(gainVal / 100.0, 'f', 2));
        if (muteCb) muteCb->setChecked(r["mute"].toBool());
    }
}

void MainWindow::onAddDestination() {
    int row = m_destTable->rowCount();
    m_destTable->insertRow(row);
    m_destTable->setRowHeight(row, 30);
    auto* nameEdit = new QLineEdit("New Stream");
    nameEdit->setMinimumWidth(140);
    m_destTable->setCellWidget(row, 0, nameEdit);
    auto* addrEdit = new QLineEdit("239.69.1.128");
    addrEdit->setMinimumWidth(180);
    m_destTable->setCellWidget(row, 1, addrEdit);
    auto* port = new QSpinBox(); port->setRange(1, 65535); port->setValue(5004);
    port->setMinimumWidth(84);
    m_destTable->setCellWidget(row, 2, centerCellWidget(port));
    auto* delBtn = new QPushButton("✕"); delBtn->setFixedSize(28, 26);
    connect(delBtn, &QPushButton::clicked, this, [this, row]() {
        m_destTable->removeRow(row);
    });
    m_destTable->setCellWidget(row, 3, centerCellWidget(delBtn));
}

void MainWindow::onAddRoute() {
    int row = m_routeTable->rowCount();
    m_routeTable->insertRow(row);
    m_routeTable->setRowHeight(row, 36);

    // Source channel — centered
    auto* src = new QSpinBox(); src->setRange(0, 15); src->setValue(0);
    src->setFixedWidth(68);
    m_routeTable->setCellWidget(row, 0, centerCellWidget(src));

    // Destination stream — centered
    auto* dst = new QSpinBox(); dst->setRange(0, 15); dst->setValue(0);
    dst->setFixedWidth(68);
    m_routeTable->setCellWidget(row, 1, centerCellWidget(dst));

    // Gain: big slider + numeric label
    auto* gainWidget = new QWidget();
    auto* gainLayout = new QHBoxLayout(gainWidget);
    gainLayout->setContentsMargins(6, 2, 6, 2);
    gainLayout->setSpacing(10);
    auto* gainSlider = new QSlider(Qt::Horizontal);
    gainSlider->setRange(0, 200);
    gainSlider->setValue(100);
    gainSlider->setMinimumWidth(280);
    auto* gainLabel = new QLabel("1.00");
    gainLabel->setFixedWidth(52);
    gainLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    gainLabel->setStyleSheet("font-size: 13px; font-weight: bold;");
    connect(gainSlider, &QSlider::valueChanged, this, [gainLabel](int v) {
        gainLabel->setText(QString::number(v / 100.0, 'f', 2));
    });
    gainLayout->addWidget(gainSlider);
    gainLayout->addWidget(gainLabel);
    m_routeTable->setCellWidget(row, 2, gainWidget);

    // Mute: checkbox, centered
    auto* muteCb = new QCheckBox();
    auto* muteW = new QWidget();
    auto* muteL  = new QHBoxLayout(muteW);
    muteL->setContentsMargins(0, 0, 0, 0);
    muteL->setAlignment(Qt::AlignCenter);
    muteL->addWidget(muteCb);
    m_routeTable->setCellWidget(row, 3, muteW);

    // Delete button — centered
    auto* delBtn = new QPushButton("✕"); delBtn->setFixedSize(28, 26);
    connect(delBtn, &QPushButton::clicked, this, [this, row]() {
        m_routeTable->removeRow(row);
    });
    m_routeTable->setCellWidget(row, 4, centerCellWidget(delBtn));
}
