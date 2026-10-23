#ifndef QT_SETTINGSOTHERPERIPHERALS_HPP
#define QT_SETTINGSOTHERPERIPHERALS_HPP

#include <QString>
#include <QWidget>
#ifdef USE_VFIO
#    include <QList>
#    include <QPair>
#    include <QString>
#endif

namespace Ui {
class SettingsOtherPeripherals;
}

#ifdef USE_VFIO
class VFIODeviceModel;
#endif

class SettingsOtherPeripherals : public QWidget {
    Q_OBJECT

public:
    explicit SettingsOtherPeripherals(QWidget *parent = nullptr);
    ~SettingsOtherPeripherals();

    int  changed();

    void restore();
    void save(int soft);

public slots:
    void onCurrentMachineChanged(int machineId);

private slots:
    void on_comboBoxRTC_currentIndexChanged(int index);
    void on_pushButtonConfigureRTC_clicked();

    void on_comboBoxMemExpCard1_currentIndexChanged(int index);
    void on_pushButtonConfigureMemExpCard1_clicked();
    void on_comboBoxMemExpCard2_currentIndexChanged(int index);
    void on_pushButtonConfigureMemExpCard2_clicked();
    void on_comboBoxMemExpCard3_currentIndexChanged(int index);
    void on_pushButtonConfigureMemExpCard3_clicked();
    void on_comboBoxMemExpCard4_currentIndexChanged(int index);
    void on_pushButtonConfigureMemExpCard4_clicked();

    void on_comboBoxIsaRomCard1_currentIndexChanged(int index);
    void on_pushButtonConfigureIsaRomCard1_clicked();
    void on_comboBoxIsaRomCard2_currentIndexChanged(int index);
    void on_pushButtonConfigureIsaRomCard2_clicked();
    void on_comboBoxIsaRomCard3_currentIndexChanged(int index);
    void on_pushButtonConfigureIsaRomCard3_clicked();
    void on_comboBoxIsaRomCard4_currentIndexChanged(int index);
    void on_pushButtonConfigureIsaRomCard4_clicked();

    void on_checkBoxUnitTester_stateChanged(int arg1);
    void on_pushButtonConfigureUT_clicked();

    void on_checkBoxKeyCard_stateChanged(int arg1);
    void on_pushButtonConfigureKeyCard_clicked();

    void on_checkBoxSoftPower_stateChanged(int arg1);
    void on_pushButtonConfigureSoftPower_clicked();

    void on_listIsaBridges_currentRowChanged(int row);
    void on_pushButtonIsaBridgeNew_clicked();
    void on_pushButtonIsaBridgeRemove_clicked();
    void on_pushButtonConfigureIsaBridge_clicked();
    void on_checkBoxIsaBridgeRom_stateChanged(int arg1);
    void on_pushButtonIsaBridgeServer_clicked();
    void on_pushButtonIsaBridgeMonitor_clicked();

private:
    /* What the External ISA Bridge tab edits for one slot. The ROM image
       and monitor command live in the slot's config section (not in the
       device's Configure dialog), so they are staged here until save(). */
    struct IsaBridgeSlot {
        bool    enabled     = false;
        bool    romEnabled  = false;
        QString romFile;
        QString serverCmd;
        bool    serverAuto  = false;
        QString monitorCmd;
        bool    monitorAuto = false;
        int     cfgChanged  = 0;
    };

    void isaBridgeLoad();
    void isaBridgeRefreshList(int selectSlot);
    void isaBridgeShowSelected();
    int  isaBridgeCurrentSlot() const;
    bool isaBridgeSlotDiffers(int slot) const;

    IsaBridgeSlot isaBridge[5];
    IsaBridgeSlot isaBridgeOrig[5];
    bool          isaBridgeUpdating = false;

    Ui::SettingsOtherPeripherals *ui;
    int                           machineId { 0 };

    int                           memexp_cfg_changed[4]      = { 0, 0, 0, 0 };
    int                           isarom_cfg_changed[4]      = { 0, 0, 0, 0 };
    int                           isartc_cfg_changed         = 0;
    int                           unittester_cfg_changed     = 0;
    int                           novell_keycard_cfg_changed = 0;
    int                           softpower_cfg_changed      = 0;
    bool                          softpower_card_enabled     = false;

    SettingsCompleter            *scRTC;

    SettingsCompleter            *scMemExpCard[4];
    SettingsCompleter            *scIsaRomCard[4];

#ifdef USE_VFIO
    /* Location and custom ROM file of each device to pass through. */
    using VFIODeviceList = QList<QPair<QString, QString>>;

    void           vfioEnumerate();
    void           vfioRestore();
    void           vfioUpdateControls();
    VFIODeviceList vfioSelectedDevices() const;
    int            vfioChanged() const;
    void           vfioSave();

    VFIODeviceModel              *vfioModel;
    bool                          vfioUpdating = false;
#endif
};

#endif // QT_SETTINGSOTHERPERIPHERALS_HPP
