/** @file
 *
 * Wireshark - Network traffic analyzer
 * By Gerald Combs <gerald@wireshark.org>
 * Copyright 1998 Gerald Combs
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef MAIN_APPLICATION_H
#define MAIN_APPLICATION_H

#include <config.h>

#include "wsutil/feature_list.h"

#include "epan/register.h"

#include "ui/help_url.h"

#include <QApplication>
#include <QDir>
#include <QFont>
#include <QIcon>
#include <QTimer>
#include <QTranslator>

#include "capture_event.h"

struct _e_prefs;

class QAction;
class QSocketNotifier;

// Recent items:
// - Read from prefs
// - Add from open file
// - Check current list
// - Signal updated item
// -
typedef struct _recent_item_status {
    QString filename;
    qint64 size;
    bool accessible;
    bool in_thread;
} recent_item_status;

class MainApplication : public QApplication
{
    Q_OBJECT
public:
    explicit MainApplication(int &argc,  char **argv);
    ~MainApplication();

    enum AppSignal {
        CaptureFilterListChanged,
        ColorsChanged,
        ColumnsChanged,
        DisplayFilterListChanged,
        FieldsChanged,
        FilterExpressionsChanged,
        LocalInterfacesChanged,
        NameResolutionChanged,
        PacketDissectionChanged,
        PreferencesChanged,
        ProfileChanging,
        RecentCapturesChanged,
        RecentPreferencesRead,
        FreezePacketList
    };

    enum MainMenuItem {
        FileOpenDialog,
        CaptureOptionsDialog
    };

    enum StatusInfo {
        FilterSyntax,
        FieldStatus,
        FileStatus,
        BusyStatus,
        ByteStatus,
        TemporaryStatus
    };

    void registerUpdate(register_action_e action, const char *message);
    void emitAppSignal(AppSignal signal);
    // Emitting app signals (PacketDissectionChanged in particular) from
    // dialogs on macOS can be problematic. Dialogs should call queueAppSignal
    // instead.
    // On macOS, nested event loops (e.g., calling a dialog with exec())
    // that call processEvents (e.g., from PacketDissectionChanged, or
    // anything with a ProgressFrame) caused issues off and on from 5.3.0
    // until 5.7.1/5.8.0. It appears to be solved after some false starts:
    // https://bugreports.qt.io/browse/QTBUG-53947
    // https://bugreports.qt.io/browse/QTBUG-56746
    // We also try to avoid exec / additional event loops as much as possible:
    // e.g., commit f67eccedd9836e6ced1f57ae9889f57a5400a3d7
    // (note it can show up in unexpected places, e.g. static functions like
    // WiresharkFileDialog::getOpenFileName())
    void queueAppSignal(AppSignal signal) { app_signals_ << signal; }
    void emitStatCommandSignal(const QString &menu_path, const char *arg, void *userdata);
    void emitTapParameterSignal(const QString cfg_abbr, const QString arg, void *userdata);
    void addDynamicMenuGroupItem(int group, QAction *sg_action);
    void appendDynamicMenuGroupItem(int group, QAction *sg_action);
    void removeDynamicMenuGroupItem(int group, QAction *sg_action);
    QList<QAction *> dynamicMenuGroupItems(int group);
    QList<QAction *> addedMenuGroupItems(int group);
    QList<QAction *> removedMenuGroupItems(int group);
    void clearAddedMenuGroupItems();
    void clearRemovedMenuGroupItems();

    void allSystemsGo();
    void emitLocalInterfaceEvent(const char *ifname, int added, int up);

    virtual void refreshLocalInterfaces();
#ifdef HAVE_LIBPCAP
    // This returns a deep copy of the cached interface list that must
    // be freed with free_interface_list.
    GList * getInterfaceList() const;
    // This set the cached interface list to a deep copy of if_list.
    void setInterfaceList(GList *if_list);
#endif

    struct _e_prefs * readConfigurationFiles(bool reset);
    QList<recent_item_status *> recentItems() const;
    void addRecentItem(const QString filename, qint64 size, bool accessible);
    void removeRecentItem(const QString &filename);
    QDir openDialogInitialDir();
    void setLastOpenDirFromFilename(QString file_name);
    void helpTopicAction(topic_action_e action);
    const QFont monospaceFont(bool zoomed = false) const;
    void setMonospaceFont(const char *font_string);
    int monospaceTextSize(const char *str);
    void setConfigurationProfile(const char *profile_name, bool write_recent_file = true);
    void reloadLuaPluginsDelayed();
    bool isInitialized() { return initialized_; }
    void setReloadingLua(bool is_reloading) { is_reloading_lua_ = is_reloading; }
    bool isReloadingLua() { return is_reloading_lua_; }
    const QIcon &normalIcon();
    const QIcon &captureIcon();
    const QString &windowTitleSeparator() const { return window_title_separator_; }
    const QString windowTitleString(QStringList title_parts);
    const QString windowTitleString(QString title_part) { return windowTitleString(QStringList() << title_part); }
    void applyCustomColorsFromRecent();
#if defined(HAVE_SOFTWARE_UPDATE) && defined(Q_OS_WIN)
    void rejectSoftwareUpdate() { software_update_ok_ = false; }
    bool softwareUpdateCanShutdown();
    void softwareUpdateShutdownRequest();
#endif
    QWidget *mainWindow();

    QTranslator translator;
    QTranslator translatorQt;
    void loadLanguage(const QString language);

    void doTriggerMenuItem(MainMenuItem menuItem);

    void zoomTextFont(int zoomLevel);

    void pushStatus(StatusInfo sinfo, const QString &message, const QString &messagetip = QString());
    void popStatus(StatusInfo sinfo);

    void gotoFrame(int frameNum);

private:
    bool initialized_;
    bool is_reloading_lua_;
    QFont mono_font_;
    QFont zoomed_font_;
    QTimer recent_timer_;
    QTimer packet_data_timer_;
    QTimer tap_update_timer_;
    QList<QString> pending_open_files_;
    QSocketNotifier *if_notifier_;
    static QString window_title_separator_;
    QList<AppSignal> app_signals_;
    int active_captures_;

#if defined(HAVE_SOFTWARE_UPDATE) && defined(Q_OS_WIN)
    bool software_update_ok_;
#endif

    void storeCustomColorsInRecent();
    void clearDynamicMenuGroupItems();

protected:
    bool event(QEvent *event);
    virtual void initializeIcons() = 0;

    QIcon normal_icon_;
    QIcon capture_icon_;
#ifdef HAVE_LIBPCAP
    GList *cached_if_list_;
#endif

signals:
    void appInitialized();
    void localInterfaceEvent(const char *ifname, int added, int up);
    void scanLocalInterfaces(GList *filter_list = nullptr);
    void localInterfaceListChanged();
    void openCaptureFile(QString cf_path, QString display_filter, unsigned int type);
    void openCaptureOptions();
    void recentPreferencesRead();
    void updateRecentCaptureStatus(const QString &filename, qint64 size, bool accessible);
    void splashUpdate(register_action_e action, const char *message);
    void profileChanging();
    void profileNameChanged(const char *profile_name);

    void freezePacketList(bool changing_profile);
    void columnsChanged(); // XXX This recreates the packet list. We might want to rename it accordingly.
    void captureFilterListChanged();
    void displayFilterListChanged();
    void filterExpressionsChanged();
    void packetDissectionChanged();
    void colorsChanged();
    void preferencesChanged();
    void addressResolutionChanged();
    void columnDataChanged();
    void checkDisplayFilter();
    void fieldsChanged();
    void reloadLuaPlugins();
#if defined(HAVE_SOFTWARE_UPDATE) && defined(Q_OS_WIN)
    // Each of these are called from a separate thread.
    void softwareUpdateRequested();
    void softwareUpdateQuit();
#endif

    void openStatCommandDialog(const QString &menu_path, const char *arg, void *userdata);
    void openTapParameterDialog(const QString cfg_str, const QString arg, void *userdata);

    /* Signals activation and stop of a capture. The value provides the number of active captures */
    void captureActive(int);

    void zoomRegularFont(const QFont & font);
    void zoomMonospaceFont(const QFont & font);

public slots:
    void clearRecentCaptures();
    void refreshRecentCaptures();

    void captureEventHandler(CaptureEvent);

    // Flush queued app signals. Should be called from the main window after
    // each dialog that calls queueAppSignal closes.
    void flushAppSignals();

    void reloadDisplayFilterMacros();

private slots:
    void updateTaps();

    void cleanup();
    void ifChangeEventsAvailable();
    void itemStatusFinished(const QString filename = "", qint64 size = 0, bool accessible = false);
    void refreshPacketData();
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0) && defined(Q_OS_WIN)
    void colorSchemeChanged();
#endif
};

extern MainApplication *mainApp;

/** Global compile time version info */
extern void gather_wireshark_qt_compiled_info(feature_list l);
/** Global runtime version info */
extern void gather_wireshark_runtime_info(feature_list l);
#endif // MAIN_APPLICATION_H
