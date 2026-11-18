import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 940
    height: 660
    minimumWidth: 600
    minimumHeight: 460
    visible: true
    title: qsTr("omacards")
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false

    onClosing: {
        // Keep the app alive in the tray; Q is the explicit quit shortcut.
        close.accepted = false;
        hide();
    }

    Item {
        id: keyHandler
        anchors.fill: parent
        focus: true
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        Keys.onPressed: function(event) {
            if (event.modifiers & Qt.ControlModifier)
                return;
            if (newDeckDialog.opened || newCardDialog.opened || importDialog.opened) {
                if (event.key === Qt.Key_Escape) {
                    newDeckDialog.close();
                    newCardDialog.close();
                    importDialog.close();
                    event.accepted = true;
                }
                return;
            }
            switch (event.key) {
            case Qt.Key_Space:
            case Qt.Key_Up:
            case Qt.Key_Down:
            case Qt.Key_Return:
                backend.flip();
                event.accepted = true;
                break;
            case Qt.Key_1:
            case Qt.Key_A:
            case Qt.Key_Left:
                backend.showingAnswer ? backend.answer(1) : backend.flip();
                event.accepted = true;
                break;
            case Qt.Key_2:
            case Qt.Key_G:
            case Qt.Key_Right:
                backend.showingAnswer ? backend.answer(3) : backend.flip();
                event.accepted = true;
                break;
            case Qt.Key_N:
                newCardDialog.open();
                event.accepted = true;
                break;
            case Qt.Key_D:
                newDeckDialog.open();
                event.accepted = true;
                break;
            case Qt.Key_I:
                importDialog.open();
                event.accepted = true;
                break;
            case Qt.Key_E:
                backend.editDeckFile();
                event.accepted = true;
                break;
            case Qt.Key_C:
                backend.setMode("cram");
                event.accepted = true;
                break;
            case Qt.Key_R:
                backend.setMode("review");
                event.accepted = true;
                break;
            case Qt.Key_Question:
                helpVisible = !helpVisible;
                event.accepted = true;
                break;
            case Qt.Key_F1:
                helpVisible = !helpVisible;
                event.accepted = true;
                break;
            case Qt.Key_Q:
                Qt.quit();
                event.accepted = true;
                break;
            case Qt.Key_Escape:
                helpVisible = false;
                event.accepted = true;
                break;
            }
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 18
            spacing: 14

            ToolBar {
                Layout.fillWidth: true
                background: Rectangle {
                    color: "#17171b"
                    radius: Math.min(4, theme.radius)
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 10

                    Label {
                        text: qsTr("Deck")
                        color: "#a8a8b0"
                    }
                    ComboBox {
                        id: deckCombo
                        Layout.preferredWidth: 210
                        model: backend.decks
                        currentIndex: Math.max(0, model.indexOf(backend.currentDeck))
                        onActivated: { backend.selectDeck(modelData); keyHandler.forceActiveFocus() }
                    }
                    Button {
                        text: qsTr("Review (R)")
                        highlighted: backend.mode === "review"
                        onClicked: { backend.setMode("review"); keyHandler.forceActiveFocus() }
                    }
                    Button {
                        text: qsTr("Cram (C)")
                        highlighted: backend.mode === "cram"
                        onClicked: { backend.setMode("cram"); keyHandler.forceActiveFocus() }
                    }
                    Item { Layout.fillWidth: true }
                    Label {
                        text: qsTr("%1 due").arg(backend.dueCount)
                        color: theme.accent
                    }
                    ToolButton {
                        text: "?"
                        onClicked: { helpVisible = !helpVisible; keyHandler.forceActiveFocus() }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: backend.status
                color: "#8e8e98"
                elide: Text.ElideRight
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                color: "#17171b"
                radius: Math.min(8, theme.radius)
                border.color: "#2a2a32"
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 28
                    spacing: 18

                    Text {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        text: backend.question
                        textFormat: Text.RichText
                        wrapMode: Text.WordWrap
                        font.pixelSize: 30
                        color: "#f2f2f4"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: backend.showingAnswer ? Text.AlignTop : Text.AlignVCenter
                        onLinkActivated: function(link) { Qt.openUrlExternally(link) }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: "#2a2a32"
                        visible: backend.showingAnswer
                    }

                    Text {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        visible: backend.showingAnswer
                        text: backend.answer
                        textFormat: Text.RichText
                        wrapMode: Text.WordWrap
                        font.pixelSize: 26
                        color: theme.accent
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: backend.flip()
                    z: 2
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Button {
                    text: qsTr("Again (1)")
                    enabled: backend.hasCard && backend.showingAnswer
                    onClicked: { backend.answer(1); keyHandler.forceActiveFocus() }
                }
                Button {
                    text: qsTr("Good (2)")
                    enabled: backend.hasCard && backend.showingAnswer
                    highlighted: true
                    onClicked: { backend.answer(3); keyHandler.forceActiveFocus() }
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: backend.statsText()
                    color: "#8e8e98"
                }
                ToolButton {
                    text: qsTr("+ Card (N)")
                    onClicked: { newCardDialog.open(); keyHandler.forceActiveFocus() }
                }
                ToolButton {
                    text: qsTr("+ Deck (D)")
                    onClicked: { newDeckDialog.open(); keyHandler.forceActiveFocus() }
                }
            }
        }
    }

    Popup {
        id: helpPopup
        parent: win.contentItem
        visible: helpVisible
        anchors.centerIn: parent
        width: Math.min(560, win.width - 40)
        height: Math.min(480, win.height - 40)
        modal: true
        padding: 24
        onClosed: { helpVisible = false; keyHandler.forceActiveFocus() }
        background: Rectangle {
            color: "#17171b"
            radius: Math.min(8, theme.radius)
            border.color: "#33333d"
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 10
            Label {
                text: qsTr("Keyboard shortcuts")
                font.pixelSize: 22
                font.bold: true
            }
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                rowSpacing: 8
                columnSpacing: 18
                Repeater {
                    model: [
                        [qsTr("Flip / main action"), "Space, ↑, ↓, Enter"],
                        [qsTr("Again"), "1, A, ←"],
                        [qsTr("Good"), "2, G, →"],
                        [qsTr("New card"), "N"],
                        [qsTr("New deck"), "D"],
                        [qsTr("Import"), "I"],
                        [qsTr("Edit deck file"), "E"],
                        [qsTr("Cram mode"), "C"],
                        [qsTr("Review mode"), "R"],
                        [qsTr("This help"), "?, F1"],
                        [qsTr("Quit"), "Q"],
                    ]
                    Label { text: modelData[0]; color: "#c8c8d0" }
                    Label { text: modelData[1]; color: theme.accent; font.bold: true }
                }
            }
            Item { Layout.fillHeight: true }
            Label {
                text: qsTr("Press Escape to close")
                color: "#8e8e98"
            }
        }
    }

    Dialog {
        id: newDeckDialog
        title: qsTr("New deck")
        standardButtons: Dialog.Ok | Dialog.Cancel
        modal: true
        anchors.centerIn: parent
        width: Math.min(420, win.width - 40)
        onAccepted: {
            backend.newDeck(deckNameField.text.trim());
            deckNameField.clear();
            keyHandler.forceActiveFocus();
        }
        TextField {
            id: deckNameField
            anchors.fill: parent
            placeholderText: qsTr("Deck name")
        }
    }

    Dialog {
        id: newCardDialog
        title: qsTr("New card")
        standardButtons: Dialog.Ok | Dialog.Cancel
        modal: true
        anchors.centerIn: parent
        width: Math.min(520, win.width - 40)
        onAccepted: {
            backend.addCard(questionField.text, answerField.text);
            questionField.clear();
            answerField.clear();
            keyHandler.forceActiveFocus();
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 8
            TextField {
                id: questionField
                Layout.fillWidth: true
                placeholderText: qsTr("Question (LaTeX with \\( ... \\))")
            }
            TextField {
                id: answerField
                Layout.fillWidth: true
                placeholderText: qsTr("Answer")
            }
        }
    }

    Dialog {
        id: importDialog
        title: qsTr("Import cards")
        standardButtons: Dialog.Ok | Dialog.Cancel
        modal: true
        anchors.centerIn: parent
        width: Math.min(520, win.width - 40)
        onAccepted: {
            backend.importFile(importPathField.text.trim(), backend.currentDeck);
            importPathField.clear();
            keyHandler.forceActiveFocus();
        }
        TextField {
            id: importPathField
            anchors.fill: parent
            placeholderText: qsTr("Path to .apkg, .csv or .tsv")
        }
    }

    Popup {
        id: noticePopup
        parent: win.contentItem
        x: (win.width - implicitWidth) / 2
        y: win.height - implicitHeight - 24
        visible: noticeTimer.running
        padding: 12
        background: Rectangle {
            color: "#2a2a32"
            radius: Math.min(6, theme.radius)
        }
        Label {
            text: backend.status
            color: "#f2f2f4"
        }
    }

    Timer {
        id: noticeTimer
        interval: 2600
    }

    Connections {
        target: backend
        function onErrorOccurred(message) { noticeTimer.restart() }
    }
}
