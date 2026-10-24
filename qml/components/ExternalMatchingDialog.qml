import QtQuick
import QtQuick.Controls.FluentWinUI3
import QtQuick.Layouts
import "../theme"

Dialog {
    id: dialog
    required property QtObject session
    parent: Overlay.overlay
    title: "为 mpv 选择外挂弹幕"
    modal: true
    focus: true
    width: parent ? Math.min(parent.width - 24, 720 * Ui.textScale) : 720
    height: parent ? Math.min(parent.height - 24, 640 * Ui.textScale) : 640
    x: parent ? (parent.width - width) / 2 : 0
    y: parent ? (parent.height - height) / 2 : 0
    closePolicy: Popup.CloseOnEscape
    onRejected: session.cancelChoice()
    Connections {
        target: dialog.session
        function onShowChoice() { dialog.open() }
        function onChanged() { if (!dialog.session.active || !dialog.session.needsChoice) dialog.close() }
    }
    footer: DialogButtonBox {
        Button { text: "取消本次弹幕"; DialogButtonBox.buttonRole: DialogButtonBox.RejectRole }
    }
    leftPadding: Ui.scrollInset(bar.implicitWidth, padding)
    rightPadding: leftPadding
    contentItem: ScrollView {
        id: scroll
        contentWidth: availableWidth
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical: AppScrollBar {
            id: bar
            viewport: scroll
            scrollTarget: scroll.contentItem
            wheelRegion: scroll.parent
            wheelBottom: dialog.footer.y
            Accessible.name: "mpv 弹幕选源滚动条"
        }
        ColumnLayout {
            width: scroll.availableWidth
            spacing: 12
            Label { text: dialog.session.title; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            Label { text: dialog.session.status; Layout.fillWidth: true; wrapMode: Text.WordWrap; Accessible.role: Accessible.AlertMessage }
            Label { text: dialog.session.error; visible: text.length > 0; Layout.fillWidth: true; wrapMode: Text.WordWrap; Accessible.role: Accessible.AlertMessage }
            Label { text: "视频继续播放。确认作品、季度和集数后会记住关联。"; Layout.fillWidth: true; wrapMode: Text.WordWrap; color: Ui.secondaryText }
            Button { text: "重新自动匹配"; onClicked: dialog.session.retry() }
            OnlineDanmakuPane {
                Layout.fillWidth: true
                library: dialog.session.library
                loadActionText: "确认并显示弹幕"
                onCancelled: { dialog.session.cancelChoice(); dialog.close() }
                onOpenSettings: dialog.session.retry()
            }
        }
    }
}
