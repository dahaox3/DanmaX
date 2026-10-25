import QtQuick
import QtQuick.Controls.FluentWinUI3
import QtQuick.Layouts
import QtQuick.Dialogs
import "../theme"

SettingsSection {
    id: pane
    required property QtObject backend
    readonly property QtObject store: backend.mpvIntegration
    title: "MPV 网页接入"
    function ensureVisible(item) {
        Qt.callLater(() => {
            if (!manager.visible) return
            const y = item.mapToItem(form, 0, 0).y
            const target = y < mpvViewport.contentY ? y : Math.max(mpvViewport.contentY, y + item.height - mpvViewport.height)
            mpvViewport.contentY = Math.max(0, Math.min(Math.max(0, mpvViewport.contentHeight - mpvViewport.height), target))
        })
    }
    function manage() {
        store.refresh()
        mpvPath.text = store.mpvPath
        serverUrl.text = store.serverUrl
        token.text = ""
        automatic.checked = store.enabled
        manager.open()
    }
    onVisibleChanged: if (!visible) manager.close()
    SettingsRow {
        title: "网页唤起 MPV 与外挂弹幕"
        description: pane.store.error.length > 0 ? pane.store.error : pane.store.status
        Button {
            id: setupButton
            objectName: "manageMpvIntegration"
            text: "配置接入"
            Accessible.name: "配置 MPV 网页接入"
            onClicked: pane.manage()
        }
        Button {
            objectName: "restoreMpvProtocols"
            text: "还原原协议"
            enabled: pane.store.hasBackup
            Accessible.name: text
            onClicked: pane.store.restoreProtocols()
        }
    }
    Dialog {
        id: manager
        objectName: "mpvIntegrationDialog"
        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        focus: true
        background: PopupSurface { }
        title: "MPV 网页接入"
        width: Math.min(720, Math.max(0, parent.width - 32))
        height: Math.min(implicitHeight, Math.max(0, parent.height - 32))
        leftPadding: Ui.scrollInset(bar.implicitWidth, padding)
        rightPadding: leftPadding
        closePolicy: Popup.CloseOnEscape
        standardButtons: Dialog.NoButton
        onOpened: mpvPath.forceActiveFocus()
        onClosed: { program.close(); token.text = ""; if (pane.visible) setupButton.forceActiveFocus(Qt.TabFocusReason) }
        footer: DialogButtonBox {
            alignment: Qt.AlignRight
            Button {
                objectName: "saveMpvIntegration"
                text: "保存并绑定协议"
                highlighted: true
                onClicked: {
                    if (pane.store.save(mpvPath.text, serverUrl.text, token.text, automatic.checked)
                        && pane.store.bindProtocols()) manager.close()
                    else Qt.callLater(() => { issue.forceActiveFocus(); pane.ensureVisible(issue) })
                }
            }
            Button { text: "取消"; objectName: "cancelMpvIntegration"; onClicked: manager.close() }
        }
        contentItem: Item {
            implicitHeight: form.implicitHeight
            implicitWidth: 620
            Flickable {
                id: mpvViewport
                objectName: "mpvIntegrationViewport"
                anchors.fill: parent
                clip: true
                contentWidth: width
                contentHeight: form.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                ColumnLayout {
                    id: form
                    width: mpvViewport.width
                    spacing: Ui.cardGap
                    Label {
                        text: "选择本机 mpv.exe，绑定后点击 Emby 网页的 MPV 即可播放。保留续播、字幕与进度回传；自动弹幕跟随本次播放器。"
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                        color: Ui.secondaryText
                    }
                    SettingsRow {
                        title: "MPV 程序"
                        description: "保留所选 mpv 目录中的原有配置和快捷键。"
                        ColumnLayout {
                            Layout.preferredWidth: Math.min(340, mpvViewport.width - 48)
                            TextField {
                                id: mpvPath
                                objectName: "mpvIntegrationPath"
                                Layout.fillWidth: true
                                selectByMouse: true
                                maximumLength: 32768
                                placeholderText: "选择 mpv.exe"
                                Accessible.name: "MPV 程序路径"
                                onFocusChanged: if (focus) pane.ensureVisible(mpvPath)
                            }
                            Button { text: "选择程序…"; Accessible.name: "选择 MPV 程序"; onClicked: program.open() }
                        }
                    }
                    SettingsRow {
                        title: "自动显示外挂弹幕"
                        description: "关闭后仍可从网页启动视频，沿用 mpv 自身弹幕设置。"
                        Switch { id: automatic; objectName: "mpvIntegrationEnabled"; Accessible.name: "自动显示外挂弹幕"; onFocusChanged: if (focus) pane.ensureVisible(automatic) }
                    }
                    SettingsRow {
                        title: "弹幕服务器"
                        description: "使用现有 Misaka 兼容服务器。"
                        TextField {
                            id: serverUrl
                            objectName: "mpvIntegrationServer"
                            Layout.preferredWidth: Math.min(340, mpvViewport.width - 48)
                            maximumLength: 2048
                            selectByMouse: true
                            placeholderText: "http://服务器:端口"
                            Accessible.name: "弹幕服务器地址"
                            onFocusChanged: if (focus) pane.ensureVisible(serverUrl)
                        }
                    }
                    SettingsRow {
                        title: "播放器 Token"
                        description: pane.store.tokenStored ? "当前服务器已保存 Token；留空保留。更换服务器时请重新填写。" : "填写服务器现有播放器 Token，配置仅限当前 Windows 用户访问。"
                        TextField {
                            id: token
                            objectName: "mpvIntegrationToken"
                            Layout.preferredWidth: Math.min(340, mpvViewport.width - 48)
                            maximumLength: 512
                            echoMode: TextInput.Password
                            Accessible.name: "弹幕播放器 Token"
                            onFocusChanged: if (focus) pane.ensureVisible(token)
                        }
                    }
                    Label {
                        id: issue
                        objectName: "mpvIntegrationIssue"
                        text: pane.store.error
                        visible: text.length > 0
                        textFormat: Text.PlainText
                        wrapMode: Text.WrapAnywhere
                        color: Ui.cautionText
                        Layout.fillWidth: true
                        Accessible.name: text
                        activeFocusOnTab: visible
                    }
                    Button {
                        text: "Windows 默认应用…"
                        visible: pane.store.error.indexOf("默认应用") >= 0
                        onClicked: pane.store.openDefaultApps()
                    }
                    Label {
                        text: "绑定只作用于当前 Windows 用户，并备份已有协议。换电脑或移动 DanmaX 后，在此重新选择程序并绑定即可。"
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                        color: Ui.secondaryText
                        Layout.fillWidth: true
                    }
                }
                ScrollBar.vertical: AppScrollBar { id: bar; objectName: "mpvIntegrationScrollBar"; Accessible.name: "MPV 接入设置滚动条"; viewport: mpvViewport; wheelRegion: manager.contentItem.parent; parent: manager.contentItem.parent; y: manager.contentItem.y; wheelTop: manager.contentItem.y; wheelBottom: manager.contentItem.y + manager.contentItem.height }
            }
        }
    }
    FileDialog {
        id: program
        title: "选择 mpv.exe"
        fileMode: FileDialog.OpenFile
        nameFilters: ["MPV 程序 (*.exe)"]
        onAccepted: if (manager.visible) mpvPath.text = pane.store.localPath(selectedFile)
    }
}
