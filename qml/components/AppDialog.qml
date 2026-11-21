import QtQuick.Controls.FluentWinUI3

Dialog {
    // Fluent's negative insets reserve space for its shader-drawn shadow.
    // Our opaque surface fills the dialog itself, even on the software backend.
    leftInset: 0
    rightInset: 0
    topInset: 0
    bottomInset: 0
    background: PopupSurface {}
}
