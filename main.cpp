#include <QApplication>
#include <QAction>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QGridLayout>
#include <QLabel>
#include <QMenu>
#include <QShowEvent>
#include <QHideEvent>
#include <QLockFile>
#include <QProcess>
#include <QProgressBar>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QVBoxLayout>
#include <QWidgetAction>
#include <QMainWindow>
#include <QCloseEvent>
#include <QStyle>
#include <QFileInfo>
#include <QCloseEvent>
#include <QMetaObject>
#include <QTimer>
#include <QCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QRegularExpression>
#include <QThread>
#include <QDebug>
#include <QDateTime>
#include <atomic>
#include <array>
#include <chrono>
#include <functional>
#include <thread>
#include <vector>
#include <map>
#include <optional>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <bluetooth/bluetooth.h>
#include <bluetooth/l2cap.h>

struct Cell { int level = 0; bool charging = false; };
struct ScanState { QString device; QString message; std::map<QString, Cell> cells; bool singleBattery = false; };

static void debugLog(const QString &message) {
    if (!qEnvironmentVariableIsSet("CACHYOS_AIRPODS_BATTERY_TRAY_DEBUG")) return;
    const QByteArray line = (QDateTime::currentDateTime().toString(Qt::ISODate) + QStringLiteral(" ") + message + QLatin1Char('\n')).toUtf8();
    if (FILE *f = std::fopen("/tmp/cachyos-airpods-battery-tray-debug.log", "a")) { std::fwrite(line.constData(), 1, size_t(line.size()), f); std::fclose(f); }
}

static QString commandOutput(const QString &program, const QStringList &args, int timeoutMs = 1000) {
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted(400)) return {};
    if (!p.waitForFinished(timeoutMs)) { p.kill(); p.waitForFinished(200); return {}; }
    return QString::fromUtf8(p.readAllStandardOutput());
}

static std::vector<std::pair<QString, QString>> connectedAirPods() {
    std::vector<std::pair<QString, QString>> result;
    const QString output = commandOutput(QStringLiteral("bluetoothctl"), {QStringLiteral("devices")}, 1200);
    const QRegularExpression line(QStringLiteral(R"(^Device ((?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}) (.+)$)"));
    for (const QString &row : output.split('\n')) {
        const auto m = line.match(row.trimmed());
        if (!m.hasMatch() || !m.captured(2).contains(QStringLiteral("airpod"), Qt::CaseInsensitive)) continue;
        const QString address = m.captured(1);
        const QString info = commandOutput(QStringLiteral("bluetoothctl"), {QStringLiteral("info"), address}, 1200);
        if (info.contains(QRegularExpression(QStringLiteral(R"(^\s*Connected: yes\s*$)"), QRegularExpression::MultilineOption)))
            result.emplace_back(address, m.captured(2).trimmed());
    }
    return result;
}

struct Headphone { QString address; QString name; };

static std::vector<Headphone> connectedHeadphones() {
    std::vector<Headphone> result;
    const QString output = commandOutput(QStringLiteral("bluetoothctl"), {QStringLiteral("devices")}, 1200);
    const QRegularExpression line(QStringLiteral(R"(^Device ((?:[0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}) (.+)$)"));
    for (const QString &row : output.split('\n')) {
        const auto m = line.match(row.trimmed());
        if (!m.hasMatch() || m.captured(2).contains(QStringLiteral("airpod"), Qt::CaseInsensitive)) continue;
        const QString address = m.captured(1);
        const QString info = commandOutput(QStringLiteral("bluetoothctl"), {QStringLiteral("info"), address}, 1200);
        if (!info.contains(QRegularExpression(QStringLiteral(R"(^\s*Connected: yes\s*$)"), QRegularExpression::MultilineOption))) continue;
        const bool audioIcon = info.contains(QRegularExpression(QStringLiteral(R"(^\s*Icon:\s*audio-(headset|headphones)\s*$)"), QRegularExpression::MultilineOption));
        const bool audioName = m.captured(2).contains(QRegularExpression(QStringLiteral("head(phone|set)|earbud"), QRegularExpression::CaseInsensitiveOption));
        if (audioIcon || audioName) result.push_back({address, m.captured(2).trimmed()});
    }
    return result;
}

static std::optional<int> headphoneBatteryPercent(const QString &address) {
    QString path = QStringLiteral("/org/bluez/hci0/dev_") + address;
    path.replace(':', '_');
    for (const QString &interfaceName : {QStringLiteral("org.bluez.Battery1"), QStringLiteral("org.bluez.Device1")}) {
        const QString property = interfaceName == QStringLiteral("org.bluez.Battery1") ? QStringLiteral("Percentage") : QStringLiteral("BatteryPercentage");
        const QString output = commandOutput(QStringLiteral("busctl"), {QStringLiteral("--system"), QStringLiteral("get-property"), QStringLiteral("org.bluez"), path, interfaceName, property}, 700);
        const auto match = QRegularExpression(QStringLiteral(R"((\d+)\s*$)")).match(output.trimmed());
        if (match.hasMatch()) {
            const int value = match.captured(1).toInt();
            if (value >= 0 && value <= 100) return value;
        }
    }
    return std::nullopt;
}

class Scanner {
public:
    explicit Scanner(std::function<void(ScanState)> update) : update_(std::move(update)) {}
    ~Scanner() { stop(); }
    void start() {
        if (running_.exchange(true)) return;
        debugLog(QStringLiteral("scanner start"));
        worker_ = std::thread([this] { run(); });
    }
    void stop() {
        if (!running_.exchange(false)) return;
        const int fd = socket_.load();
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        if (worker_.joinable()) worker_.join();
    }
private:
    void publish(ScanState s) {
        auto cb = update_;
        QMetaObject::invokeMethod(qApp, [cb, s = std::move(s)] { cb(s); }, Qt::QueuedConnection);
    }
    bool waitBriefly(int ms) {
        for (int i = 0; i < ms && running_; i += 100) QThread::msleep(100);
        return running_;
    }
    static bool sendPacket(int fd, const std::vector<unsigned char> &p) {
        return ::send(fd, p.data(), p.size(), MSG_NOSIGNAL) == ssize_t(p.size());
    }
    void run() {
        while (running_) {
            const auto devices = connectedAirPods();
            debugLog(QStringLiteral("connected AirPods %1").arg(devices.size()));
            if (devices.empty()) {
                const auto headphones = connectedHeadphones();
                if (!headphones.empty()) {
                    const auto &headphone = headphones.front();
                    while (running_) {
                        const auto level = headphoneBatteryPercent(headphone.address);
                        if (level) publish({headphone.name, QStringLiteral("Battery status is up to date"), {{QStringLiteral("Headphone"), {*level, false}}}, true});
                        else publish({headphone.name, QStringLiteral("Connected · waiting for headphone battery info…"), {}, true});
                        if (!waitBriefly(1800)) break;
                        const auto stillConnected = commandOutput(QStringLiteral("bluetoothctl"), {QStringLiteral("info"), headphone.address}, 700);
                        if (!stillConnected.contains(QRegularExpression(QStringLiteral(R"(^\s*Connected: yes\s*$)"), QRegularExpression::MultilineOption))) break;
                    }
                    continue;
                }
                publish({{}, QStringLiteral("Searching for connected headphones"), {}});
                if (!waitBriefly(1200)) break;
                continue;
            }
            bool opened = false;
            for (const auto &[address, name] : devices) {
                if (!running_) break;
                bdaddr_t mac{};
                const QByteArray addr = address.toLatin1();
                if (str2ba(addr.constData(), &mac) != 0) continue;
                const int fd = ::socket(AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_CLOEXEC, BTPROTO_L2CAP);
                if (fd < 0) continue;
                socket_.store(fd);
                sockaddr_l2 peer{};
                peer.l2_family = AF_BLUETOOTH;
                peer.l2_psm = htobs(0x1001);
                peer.l2_bdaddr = mac;
                peer.l2_bdaddr_type = BDADDR_BREDR;
                timeval timeout{1, 0};
                ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                if (::connect(fd, reinterpret_cast<sockaddr *>(&peer), sizeof(peer)) != 0) {
                    debugLog(QStringLiteral("L2CAP connect failed %1 %2").arg(errno).arg(strerror(errno)));
                    socket_.store(-1); ::close(fd); continue;
                }
                debugLog(QStringLiteral("L2CAP connected %1").arg(address));
                opened = true;
                publish({name, QStringLiteral("Connected · reading battery info…"), {}});
                const std::vector<std::vector<unsigned char>> init = {
                    {0x00,0x00,0x04,0x00,0x01,0x00,0x02,0x00,0,0,0,0,0,0,0,0},
                    {0x04,0x00,0x04,0x00,0x4d,0x00,0xff,0x00,0,0,0,0,0,0},
                    {0x04,0x00,0x04,0x00,0x0f,0x00,0xff,0xff,0xff,0xff}
                };
                bool sentAll = true;
                for (const auto &p : init) if (!sendPacket(fd, p)) { sentAll = false; break; }
                debugLog(QStringLiteral("AAP init sent %1").arg(sentAll));
                if (!sentAll) { socket_.store(-1); ::close(fd); continue; }
                const auto connectedAt = std::chrono::steady_clock::now();
                bool gotBattery = false;
                while (running_) {
                    pollfd wait{fd, POLLIN, 0};
                    const int ready = ::poll(&wait, 1, 400);
                    if (!running_) break;
                    if (ready > 0 && (wait.revents & POLLIN)) {
                        std::array<unsigned char, 2048> frame{};
                        const ssize_t n = ::recv(fd, frame.data(), frame.size(), 0);
                        if (n <= 0) break;
                        debugLog(QStringLiteral("AAP RX %1").arg(QByteArray(reinterpret_cast<const char *>(frame.data()), n).toHex()));
                        if (n < 7 || frame[0] != 0x04 || frame[1] != 0x00 || frame[2] != 0x04 || frame[3] != 0x00 || frame[4] != 0x04) continue;
                        std::map<QString, Cell> cells;
                        for (int i = 0; i < frame[6]; ++i) {
                            const int at = 7 + i * 5;
                            if (at + 4 >= n) break;
                            QString label;
                            switch (frame[at]) {
                                case 0x02: label = QStringLiteral("Right"); break;
                                case 0x04: label = QStringLiteral("Left"); break;
                                case 0x08: label = QStringLiteral("Case"); break;
                                default: continue;
                            }
                            const int status = frame[at + 3];
                            if (status == 0x04 || frame[at + 2] > 100) continue;
                            cells[label] = {int(frame[at + 2]), status == 0x01};
                        }
                        if (!cells.empty()) {
                            debugLog(QStringLiteral("battery parsed %1").arg(cells.size()));
                            gotBattery = true;
                            publish({name, QStringLiteral("Battery status is up to date"), std::move(cells)});
                        }
                    }
                    if (wait.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
                    const auto now = std::chrono::steady_clock::now();
                    if (!gotBattery && now - connectedAt > std::chrono::seconds(7)) {
                        publish({name, QStringLiteral("No battery response · retrying…"), {}});
                        break;
                    }
                }
                socket_.store(-1);
                ::shutdown(fd, SHUT_RDWR); ::close(fd);
            }
            if (!running_) break;
            if (!opened) publish({devices.front().second, QStringLiteral("Connected; could not open battery channel"), {}});
            if (!waitBriefly(1200)) break;
        }
        socket_.store(-1);
    }
    std::function<void(ScanState)> update_;
    std::atomic_bool running_{false};
    std::atomic_int socket_{-1};
    std::thread worker_;
};

class BatteryWindow : public QMainWindow {
public:
    explicit BatteryWindow(QWidget *parent = nullptr) : QMainWindow(parent) {
        setObjectName(QStringLiteral("batteryWindow"));
        setWindowTitle(QStringLiteral("CachyOS AirPods Battery Tray"));
        setWindowIcon(QIcon::fromTheme(QStringLiteral("audio-headphones")));
        resize(520, 250);
        setMinimumSize(460, 220);
        auto *body = new QWidget(this);
        body->setMinimumWidth(460);
        layout_ = new QVBoxLayout(body);
        layout_->setContentsMargins(16, 14, 16, 14);
        layout_->setSpacing(8);
        layout_->addStretch(1);
        title_ = new QLabel(QStringLiteral("AirPods Battery Status"), body);
        QFont titleFont = title_->font(); titleFont.setPointSize(titleFont.pointSize() + 2); titleFont.setBold(true); title_->setFont(titleFont);
        status_ = new QLabel(QStringLiteral("Scanning will start when the panel opens"), body);
        status_->setStyleSheet(QStringLiteral("color: palette(mid);"));
        layout_->addWidget(title_); layout_->addWidget(status_);
        grid_ = new QGridLayout(); grid_->setSpacing(8);
        const QStringList labels{QStringLiteral("Left"), QStringLiteral("Right"), QStringLiteral("Case"), QStringLiteral("Headphone")};
        for (int i = 0; i < labels.size(); ++i) {
            auto *card = new QWidget(body); card->setObjectName(QStringLiteral("batteryCard"));
            auto *cardLayout = new QVBoxLayout(card); cardLayout->setContentsMargins(10,8,10,8); cardLayout->setSpacing(3);
            auto *name = new QLabel(labels[i], card); name->setAlignment(Qt::AlignCenter);
            auto *value = new QLabel(QStringLiteral("—"), card); value->setAlignment(Qt::AlignCenter);
            QFont valueFont = value->font(); valueFont.setPointSize(18); valueFont.setBold(true); value->setFont(valueFont);
            auto *charge = new QLabel(QString(), card); charge->setAlignment(Qt::AlignCenter); charge->setMinimumHeight(16);
            cardLayout->addWidget(name); cardLayout->addWidget(value); cardLayout->addWidget(charge);
            grid_->addWidget(card, 0, i < 3 ? i : 0, 1, i < 3 ? 1 : 3);
            cards_[labels[i]] = card;
            if (labels[i] == QStringLiteral("Headphone")) card->hide();
            values_[labels[i]] = value; charging_[labels[i]] = charge;
        }
        layout_->addLayout(grid_);
        hint_ = new QLabel(QStringLiteral("Scanning only runs while this panel is open"), body);
        hint_->setStyleSheet(QStringLiteral("color: palette(mid);"));
        hint_->setAlignment(Qt::AlignCenter);
        layout_->addWidget(hint_);
        layout_->addStretch(1);
        setCentralWidget(body);
        setStyleSheet(QStringLiteral("QWidget#batteryCard { border: 1px solid palette(midlight); border-radius: 8px; }"));
    }
    void setVisibilityHandlers(std::function<void()> shown, std::function<void()> hidden) {
        shown_ = std::move(shown); hidden_ = std::move(hidden);
    }
    void showEvent(QShowEvent *event) override {
        QWidget::showEvent(event);
        if (shown_) shown_();
    }
    void hideEvent(QHideEvent *event) override {
        QWidget::hideEvent(event);
        if (hidden_) hidden_();
    }
    void closeEvent(QCloseEvent *event) override {
        event->ignore();
        hide();
    }
    void updateState(const ScanState &s) {
        const bool searching = s.device.isEmpty();
        if (!searching) title_->setText(s.device);
        title_->setVisible(!searching);
        status_->setText(s.message);
        status_->setAlignment(searching ? Qt::AlignCenter : Qt::AlignLeft | Qt::AlignVCenter);
        QFont statusFont = status_->font();
        statusFont.setPointSize(searching ? 15 : 10);
        statusFont.setWeight(searching ? QFont::DemiBold : QFont::Normal);
        status_->setFont(statusFont);
        status_->setStyleSheet(searching ? QStringLiteral("color: palette(text);") : QStringLiteral("color: palette(mid);"));
        hint_->setVisible(!searching);
        for (const QString &label : {QStringLiteral("Left"), QStringLiteral("Right"), QStringLiteral("Case")}) {
            cards_[label]->setVisible(!searching && !s.singleBattery);
            values_[label]->setText(QStringLiteral("—")); charging_[label]->clear();
        }
        cards_[QStringLiteral("Headphone")]->setVisible(!searching && s.singleBattery);
        values_[QStringLiteral("Headphone")]->setText(QStringLiteral("—")); charging_[QStringLiteral("Headphone")]->clear();
        for (const auto &[label, cell] : s.cells) {
            if (!values_.contains(label)) continue;
            values_[label]->setText(QStringLiteral("%1%").arg(cell.level));
            charging_[label]->setText(cell.charging ? QStringLiteral("Charging") : QString());
        }
    }
private:
    QLabel *title_{}; QLabel *status_{};
    QLabel *hint_{};
    QVBoxLayout *layout_{};
    QGridLayout *grid_{};
    QMap<QString, QLabel *> values_, charging_;
    QMap<QString, QWidget *> cards_;
    std::function<void()> shown_, hidden_;
};

static QString autostartPath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + QStringLiteral("/autostart/cachyos-airpods-battery-tray.desktop");
}
static bool autostartEnabled() { return QFile::exists(autostartPath()); }
static bool setAutostart(bool enabled) {
    const QString path = autostartPath();
    if (!enabled) return !QFile::exists(path) || QFile::remove(path);
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;
    const QString exe = QCoreApplication::applicationFilePath();
    const QByteArray data = QStringLiteral("[Desktop Entry]\nType=Application\nName=CachyOS AirPods Battery Tray\nComment=Show AirPods battery status in the system tray\nExec=\"%1\"\nIcon=audio-headphones\nTerminal=false\nX-KDE-autostart-after=panel\n").arg(exe).toUtf8();
    return f.write(data) == data.size();
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("CachyOS AirPods Battery Tray"));
    QCoreApplication::setOrganizationName(QStringLiteral("CachyOS"));
    if (!QSystemTrayIcon::isSystemTrayAvailable()) { qCritical() << "KDE system tray is unavailable."; return 1; }

    const QString lockPath = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + QStringLiteral("/cachyos-airpods-battery-tray.lock");
    QLockFile instanceLock(lockPath);
    instanceLock.setStaleLockTime(0);
    if (!instanceLock.tryLock()) return 0;

    BatteryWindow batteryWindow;
    QMenu contextMenu;
    QAction openAction(QStringLiteral("Open battery panel"), &contextMenu);
    QAction startupAction(QStringLiteral("Launch at login"), &contextMenu);
    startupAction.setCheckable(true); startupAction.setChecked(autostartEnabled());
    QAction quitAction(QStringLiteral("Quit completely"), &contextMenu);
    contextMenu.addAction(&openAction); contextMenu.addSeparator();
    contextMenu.addAction(&startupAction); contextMenu.addSeparator(); contextMenu.addAction(&quitAction);
    QSystemTrayIcon tray(QIcon::fromTheme(QStringLiteral("audio-headphones")));
    if (tray.icon().isNull()) tray.setIcon(QApplication::style()->standardIcon(QStyle::SP_MediaVolume));
    tray.setToolTip(QStringLiteral("CachyOS AirPods Battery Tray"));
    tray.setContextMenu(&contextMenu); tray.show();

    Scanner scanner([&batteryWindow](ScanState s) { batteryWindow.updateState(s); });
    auto showPanel = [&] {
        batteryWindow.showNormal();
        batteryWindow.raise();
        batteryWindow.activateWindow();
    };
    QObject::connect(&openAction, &QAction::triggered, &app, showPanel);
    QObject::connect(&startupAction, &QAction::toggled, &app, [](bool enabled) {
        if (!setAutostart(enabled)) qWarning() << "Could not save the launch-at-login setting.";
    });
    QObject::connect(&quitAction, &QAction::triggered, &app, [&] { scanner.stop(); app.quit(); });
    batteryWindow.setVisibilityHandlers([&] {
        batteryWindow.updateState({{}, QStringLiteral("Searching for connected headphones"), {}});
        scanner.start();
    }, [&] { scanner.stop(); });
    QObject::connect(&tray, &QSystemTrayIcon::activated, &app, [&](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Context) return;
        showPanel();
    });
    if (app.arguments().contains(QStringLiteral("--show"))) QTimer::singleShot(0, &app, showPanel);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&] { scanner.stop(); tray.hide(); });
    return app.exec();
}
