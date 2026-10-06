/*
 * InputLeap -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2008 Volker Lanz (vl@fidra.de)
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ServerConfig.h"
#include "Hotkey.h"
#include "MainWindow.h"
#include "AddClientDialog.h"

#include <QtCore>
#include <cmath>
#include <limits>
#include <QMessageBox>
#include <QAbstractButton>
#include <QPushButton>

static const struct
{
     int x;
     int y;
     const char* name;
} neighbourDirs[] =
{
    {  1,  0, "right" },
    { -1,  0, "left" },
    {  0, -1, "up" },
    {  0,  1, "down" },

};

const int serverDefaultIndex = 7;

ServerConfig::ServerConfig(QSettings* settings, int numColumns, int numRows ,
                QString serverName, MainWindow* mainWindow) :
    m_pSettings(settings),
    m_Screens(),
    m_NumColumns(numColumns),
    m_NumRows(numRows),
    m_ServerName(serverName),
    m_IgnoreAutoConfigClient(false),
    m_EnableDragAndDrop(false),
    m_ClipboardSharing(true),
    m_ClipboardSharingSize(defaultClipboardSharingSize()),
    m_GameMode(false),
    m_pMainWindow(mainWindow)
{
    Q_ASSERT(m_pSettings);

    loadSettings();
}

ServerConfig::~ServerConfig()
{
    saveSettings();
}

bool ServerConfig::save(const QString& fileName) const
{
    QFile file(fileName);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    save(file);
    file.close();

    return true;
}

void ServerConfig::save(QFile& file) const
{
    QTextStream outStream(&file);
    outStream << *this;
}

void ServerConfig::init()
{
    switchCorners().clear();
    screens().clear();
    hotkeys().clear();

    // m_NumSwitchCorners is used as a fixed size array. See Screen::init()
    for (int i = 0; i < static_cast<int>(SwitchCorner::Count); i++) {
        switchCorners() << false;
    }

    // There must always be screen objects for each cell in the screens QList. Unused screens
    // are identified by having an empty name.
    for (int i = 0; i < numColumns() * numRows(); i++)
        addScreen(Screen());
}

void ServerConfig::saveSettings()
{
    settings().beginGroup("internalConfig");
    settings().remove("");

    settings().setValue("numColumns", numColumns());
    settings().setValue("numRows", numRows());

    settings().setValue("hasHeartbeat", hasHeartbeat());
    settings().setValue("heartbeat", heartbeat());
    settings().setValue("relativeMouseMoves", relativeMouseMoves());
    settings().setValue("screenSaverSync", screenSaverSync());
    settings().setValue("win32KeepForeground", win32KeepForeground());
    settings().setValue("hasSwitchDelay", hasSwitchDelay());
    settings().setValue("switchDelay", switchDelay());
    settings().setValue("hasSwitchDoubleTap", hasSwitchDoubleTap());
    settings().setValue("switchDoubleTap", switchDoubleTap());
    settings().setValue("switchCornerSize", switchCornerSize());
    settings().setValue("ignoreAutoConfigClient", ignoreAutoConfigClient());
    settings().setValue("enableDragAndDrop", enableDragAndDrop());
    settings().setValue("clipboardSharing", clipboardSharing());
    settings().setValue("clipboardSharingSize", (int)clipboardSharingSize());
    settings().setValue("gameMode", gameMode());

    writeSettings<bool>(settings(), switchCorners(), "switchCorner");

    settings().beginWriteArray("screens");
    for (std::size_t i = 0; i < screens().size(); i++)
    {
        settings().setArrayIndex(static_cast<int>(i));
        screens()[i].saveSettings(settings());
    }
    settings().endArray();

    settings().beginWriteArray("hotkeys");
    for (std::size_t i = 0; i < hotkeys().size(); i++)
    {
        settings().setArrayIndex(static_cast<int>(i));
        hotkeys()[i].saveSettings(settings());
    }
    settings().endArray();

    settings().endGroup();
}

void ServerConfig::loadSettings()
{
    settings().beginGroup("internalConfig");

    setNumColumns(settings().value("numColumns", 5).toInt());
    setNumRows(settings().value("numRows", 3).toInt());

    // we need to know the number of columns and rows before we can set up ourselves
    init();

    haveHeartbeat(settings().value("hasHeartbeat", false).toBool());
    setHeartbeat(settings().value("heartbeat", 5000).toInt());
    setRelativeMouseMoves(settings().value("relativeMouseMoves", false).toBool());
    setScreenSaverSync(settings().value("screenSaverSync", true).toBool());
    setWin32KeepForeground(settings().value("win32KeepForeground", false).toBool());
    haveSwitchDelay(settings().value("hasSwitchDelay", false).toBool());
    setSwitchDelay(settings().value("switchDelay", 250).toInt());
    haveSwitchDoubleTap(settings().value("hasSwitchDoubleTap", false).toBool());
    setSwitchDoubleTap(settings().value("switchDoubleTap", 250).toInt());
    setSwitchCornerSize(settings().value("switchCornerSize").toInt());
    setIgnoreAutoConfigClient(settings().value("ignoreAutoConfigClient").toBool());
    setEnableDragAndDrop(settings().value("enableDragAndDrop", true).toBool());
    setClipboardSharing(settings().value("clipboardSharing", true).toBool());
    setClipboardSharingSize(settings().value("clipboardSharingSize",
        (int) ServerConfig::defaultClipboardSharingSize()).toULongLong());
    setGameMode(settings().value("gameMode", false).toBool());

    readSettings<bool>(settings(), switchCorners(), "switchCorner", false,
                       static_cast<int>(SwitchCorner::Count));

    std::size_t numScreens = settings().beginReadArray("screens");
    Q_ASSERT(numScreens <= screens().size());
    for (std::size_t i = 0; i < numScreens; i++)
    {
        settings().setArrayIndex(static_cast<int>(i));
        screens()[i].loadSettings(settings());
    }
    settings().endArray();

    int numHotkeys = settings().beginReadArray("hotkeys");
    for (int i = 0; i < numHotkeys; i++)
    {
        settings().setArrayIndex(i);
        Hotkey h;
        h.loadSettings(settings());
        hotkeys().push_back(h);
    }
    settings().endArray();

    settings().endGroup();
}

int ServerConfig::adjacentScreenIndex(int idx, int deltaColumn, int deltaRow) const
{
    if (screens()[idx].isNull())
        return -1;

    // if we're at the left or right end of the table, don't find results going further left or right
    if ((deltaColumn > 0 && (idx+1) % numColumns() == 0)
            || (deltaColumn < 0 && idx % numColumns() == 0))
        return -1;

    int arrayPos = idx + deltaColumn + deltaRow * numColumns();

    if (arrayPos >= static_cast<int>(screens().size()) || arrayPos < 0)
        return -1;

    return arrayPos;
}

QTextStream& operator<<(QTextStream& outStream, const ServerConfig& config)
{
    outStream << "section: screens\n";

    for (const Screen& s : config.screens()) {
        if (!s.isNull())
            s.writeScreensSection(outStream);
    }

    outStream << "end\n\n";

    outStream << "section: aliases\n";

    for (const Screen& s : config.screens()) {
        if (!s.isNull())
            s.writeAliasesSection(outStream);
    }

    outStream << "end\n\n";

    outStream << "section: links\n";

    for (std::size_t i = 0; i < config.screens().size(); i++)
        if (!config.screens()[i].isNull())
        {
            outStream << "\t" << config.screens()[i].name() << ":\n";

            for (unsigned int j = 0; j < sizeof(neighbourDirs) / sizeof(neighbourDirs[0]); j++)
            {
                int idx = config.adjacentScreenIndex(static_cast<int>(i),
                                                     neighbourDirs[j].x, neighbourDirs[j].y);
                if (idx != -1 && !config.screens()[idx].isNull())
                    outStream << "\t\t" << neighbourDirs[j].name << " = " << config.screens()[idx].name() << "\n";
            }
        }

    outStream << "end\n\n";

    outStream << "section: options\n";

    if (config.hasHeartbeat())
        outStream << "\t" << "heartbeat = " << config.heartbeat() << "\n";

    // game mode implies relative mouse moves (written once, whichever way it was enabled)
    outStream << "\t" << "relativeMouseMoves = "
              << (config.relativeMouseMoves() || config.gameMode() ? "true" : "false") << "\n";
    outStream << "\t" << "screenSaverSync = " << (config.screenSaverSync() ? "true" : "false") << "\n";
    outStream << "\t" << "win32KeepForeground = " << (config.win32KeepForeground() ? "true" : "false") << "\n";
    outStream << "\t" << "clipboardSharing = " << (config.clipboardSharing() ? "true" : "false") << "\n";
    outStream << "\t" << "clipboardSharingSize = " << config.clipboardSharingSize() << "\n";

    if (config.hasSwitchDelay())
        outStream << "\t" << "switchDelay = " << config.switchDelay() << "\n";

    if (config.hasSwitchDoubleTap())
        outStream << "\t" << "switchDoubleTap = " << config.switchDoubleTap() << "\n";

    outStream << "\t" << "switchCorners = none ";
    for (int i = 0; i < config.switchCorners().size(); i++) {
        auto corner = static_cast<Screen::SwitchCorner>(i);
        if (config.switchCorners()[i]) {
            outStream << "+" << config.switchCornerName(corner) << " ";
        }
    }
    outStream << "\n";

    outStream << "\t" << "switchCornerSize = " << config.switchCornerSize() << "\n";

    bool hasLockHotkey = false;
    for (const Hotkey& hotkey : config.hotkeys()) {
        outStream << hotkey;
        for (const Action& action : hotkey.actions()) {
            if (action.type() == Action::lockCursorToScreen &&
                action.lockCursorMode() != Action::lockCursorOff)
                hasLockHotkey = true;
        }
    }

    // Control+Shift only: Alt/Super are swapped on a Mac server. Lowercase 'g' is the
    // spelling KeySequence writes for letter keys.
    if (config.gameMode() && !hasLockHotkey)
        outStream << "\t" << "keystroke(Control+Shift+g) = lockCursorToScreen(toggle)" << "\n";

    outStream << "end\n\n";

    return outStream;
}

int ServerConfig::numScreens() const
{
    int rval = 0;

    for (const Screen& s : screens()) {
        if (!s.isNull())
            rval++;
    }

    return rval;
}

int ServerConfig::autoAddScreen(const QString name)
{
    int serverIndex = -1;
    int targetIndex = -1;
    if (!findScreenName(m_ServerName, serverIndex)) {
        if (!fixNoServer(m_ServerName, serverIndex)) {
            return kAutoAddScreenManualServer;
        }
    }
    if (findScreenName(name, targetIndex)) {
        // already exists.
        return kAutoAddScreenIgnore;
    }

    int result = showAddClientDialog(name);

    if (result == kAddClientIgnore) {
        return kAutoAddScreenIgnore;
    }

    if (result == kAddClientOther) {
        addToFirstEmptyGrid(name);
        return kAutoAddScreenManualClient;
    }

    bool success = false;
    int startIndex = serverIndex;
    int offset = 1;
    int dirIndex = 0;

    if (result == kAddClientLeft) {
        offset = -1;
        dirIndex = 1;
    }
    else if (result == kAddClientUp) {
        offset = -5;
        dirIndex = 2;
    }
    else if (result == kAddClientDown) {
        offset = 5;
        dirIndex = 3;
    }


    int idx = adjacentScreenIndex(startIndex, neighbourDirs[dirIndex].x,
                    neighbourDirs[dirIndex].y);
    while (idx != -1) {
        if (screens()[idx].isNull()) {
            m_Screens[idx].setName(name);
            success = true;
            break;
        }

        startIndex += offset;
        idx = adjacentScreenIndex(startIndex, neighbourDirs[dirIndex].x,
                    neighbourDirs[dirIndex].y);
    }

    if (!success) {
        addToFirstEmptyGrid(name);
        return kAutoAddScreenManualClient;
    }

    saveSettings();
    return kAutoAddScreenOk;
}

bool ServerConfig::findScreenName(const QString& name, int& index)
{
    bool found = false;
    for (std::size_t i = 0; i < screens().size(); i++) {
        if (!screens()[i].isNull() &&
            screens()[i].name().compare(name) == 0) {
            index = static_cast<int>(i);
            found = true;
            break;
        }
    }
    return found;
}

bool ServerConfig::fixNoServer(const QString& name, int& index)
{
    bool fixed = false;
    if (screens()[serverDefaultIndex].isNull()) {
        m_Screens[serverDefaultIndex].setName(name);
        index = serverDefaultIndex;
        fixed = true;
    }

    return fixed;
}

int ServerConfig::showAddClientDialog(const QString& clientName)
{
    int result = kAddClientIgnore;

    if (!m_pMainWindow->isActiveWindow()) {
        m_pMainWindow->showNormal();
        m_pMainWindow->activateWindow();
    }

    AddClientDialog addClientDialog(clientName, m_pMainWindow);
    addClientDialog.exec();
    result = addClientDialog.addResult();
    m_IgnoreAutoConfigClient = addClientDialog.ignoreAutoConfigClient();

    return result;
}

void::ServerConfig::addToFirstEmptyGrid(const QString &clientName)
{
    for (std::size_t i = 0; i < screens().size(); i++) {
        if (screens()[i].isNull()) {
            m_Screens[i].setName(clientName);
            break;
        }
    }
}

size_t ServerConfig::defaultClipboardSharingSize() {
    return 100 * 1000 * 1000; // 100 MB
}

size_t ServerConfig::setClipboardSharingSize(size_t size) {
    using std::swap;
    swap (size, m_ClipboardSharingSize);
    return size;
}

bool ServerConfig::hasScreen(const QString& name) const
{
    for (const Screen& s : screens()) {
        if (!s.isNull() && (s.name() == name || s.aliases().contains(name)))
            return true;
    }
    return false;
}

QVariantMap ServerConfig::toVariantMap()
{
    saveSettings();

    QVariantMap map;
    settings().beginGroup("internalConfig");
    const QStringList keys = settings().allKeys();
    for (const QString& key : keys)
        map.insert(key, settings().value(key));
    settings().endGroup();
    return map;
}

namespace {

const int kMaxKeys = 4000;
const int kMaxKeyLength = 64;
const int kMaxStringLength = 256;
const int kMaxGridCells = 100;
const int kMaxArraySize = 64;       // per hotkey/action/alias/... array
const int kMaxArraySizeTotal = 4000; // all "/size" values together: bounds loadSettings() work

bool isScalar(const QVariant& v)
{
    switch (v.userType()) {
    case QMetaType::Bool:
    case QMetaType::Int:
    case QMetaType::UInt:
    case QMetaType::LongLong:
    case QMetaType::ULongLong:
        return true;
    case QMetaType::Double:
        return std::isfinite(v.toDouble());
    case QMetaType::QString:
        // control characters would let a value break out of its line in the generated config
        for (const QChar c : v.toString()) {
            if (c.unicode() < 0x20 || c.unicode() == 0x7f)
                return false;
        }
        return v.toString().size() <= kMaxStringLength;
    default:
        return false;
    }
}

bool intValue(const QVariant& v, int& out)
{
    bool ok = false;
    const qlonglong n = v.toLongLong(&ok);
    if (!ok || n < std::numeric_limits<int>::min() || n > std::numeric_limits<int>::max())
        return false;
    out = static_cast<int>(n);
    return true;
}

// Returns "" when the map is safe to hand to loadSettings(), else a Korean reason.
QString checkLayoutMap(const QVariantMap& map)
{
    const QString bad = QStringLiteral("받은 화면 배치 형식이 올바르지 않습니다");

    if (map.size() > kMaxKeys)
        return QStringLiteral("받은 화면 배치가 너무 큽니다");

    int sizeTotal = 0;
    QSet<QString> seen;
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        const QString& key = it.key();
        if (key.isEmpty() || key.size() > kMaxKeyLength)
            return bad;
        for (const QChar c : key) {
            const ushort u = c.unicode();
            const bool ok = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
                            (u >= '0' && u <= '9') || u == '_' || u == '/' || u == ' ' || u == '-';
            if (!ok)
                return bad;
        }
        // QSettings normalizes keys ('//' collapsed, edge '/' stripped) and the Windows registry
        // ignores case: such keys would overwrite validated ones, so refuse them
        if (key.startsWith(QLatin1Char('/')) || key.endsWith(QLatin1Char('/')) ||
            key.contains(QLatin1String("//")) || seen.contains(key.toLower()))
            return bad;
        seen.insert(key.toLower());
        if (!isScalar(it.value()))
            return bad;

        // enum-like values are used as array indexes later, keep them in range
        const QString last = key.section('/', -1);
        int n = 0;
        if (key.endsWith(QLatin1String("/size"))) {
            if (!intValue(it.value(), n) || n < 0 ||
                n > (key == QLatin1String("screens/size") ? kMaxGridCells : kMaxArraySize))
                return bad;
            sizeTotal += n;
            if (sizeTotal > kMaxArraySizeTotal)
                return bad;
        }
        else if (last == QLatin1String("type")) {   // Action::ActionType
            if (!intValue(it.value(), n) || n < 0 || n > static_cast<int>(Action::lockCursorToScreen))
                return bad;
        }
        else if (last == QLatin1String("switchInDirection")) {
            if (!intValue(it.value(), n) || n < 0 || n > static_cast<int>(Action::switchDown))
                return bad;
        }
        else if (last == QLatin1String("lockCursorToScreen")) {
            if (!intValue(it.value(), n) || n < 0 || n > static_cast<int>(Action::lockCursorOff))
                return bad;
        }
        else if (last == QLatin1String("modifier")) {
            if (!intValue(it.value(), n) || n < static_cast<int>(BaseConfig::Modifier::DefaultMod) ||
                n >= static_cast<int>(BaseConfig::Modifier::Count))
                return bad;
        }
    }

    // the screens array must match the grid exactly: loadSettings() trusts it as an index bound
    int columns = 0, rows = 0, screens = 0;
    if (!intValue(map.value(QStringLiteral("numColumns")), columns) ||
        !intValue(map.value(QStringLiteral("numRows")), rows) ||
        !intValue(map.value(QStringLiteral("screens/size")), screens) ||
        columns <= 0 || rows <= 0 || columns > kMaxGridCells || rows > kMaxGridCells ||
        columns * rows > kMaxGridCells || screens != columns * rows)
        return bad;

    // Action::text() indexes a name table with type (+ mouse offset): a non-mouse action
    // whose last key looks like a mouse button (< Key_Space, incl. a missing entry) would overrun it
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        if (!it.key().startsWith(QLatin1String("hotkeys/")) ||
            !it.key().endsWith(QLatin1String("/type")))
            continue;
        const QString prefix = it.key().left(it.key().size() - 4);
        int type = 0, count = 0;
        intValue(it.value(), type);
        if (type > static_cast<int>(Action::keystroke) &&
            intValue(map.value(prefix + QStringLiteral("keys/size")), count) && count > 0 &&
            map.value(prefix + QStringLiteral("keys/") + QString::number(count) +
                      QStringLiteral("/key")).toInt() < static_cast<int>(Qt::Key_Space))
            return bad;
    }

    return QString();
}

} // namespace

bool ServerConfig::fromVariantMap(const QVariantMap& map, QString* error)
{
    const QString why = checkLayoutMap(map);
    if (!why.isEmpty()) {
        if (error)
            *error = why;
        return false;
    }

    settings().beginGroup("internalConfig");
    settings().remove("");
    for (auto it = map.cbegin(); it != map.cend(); ++it) {
        QVariant v = it.value();
        // JSON carries every number as double; store whole numbers as integers again
        if (v.userType() == QMetaType::Double && std::floor(v.toDouble()) == v.toDouble() &&
            std::fabs(v.toDouble()) < 9e15)
            v = static_cast<qlonglong>(v.toDouble());
        settings().setValue(it.key(), v);
    }
    settings().endGroup();

    loadSettings();
    return true;
}
