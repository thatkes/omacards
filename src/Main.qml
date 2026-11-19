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
    property bool statsVisible: false
    property var statsData: ({})
    property string editCardId: ""

    function openStats() {
        statsData = backend.statsSummary()
        statsVisible = true
    }

    function openEditor(card) {
        editCardId = card.id
        editQuestion.text = card.question
        editAnswer.text = card.answer
        editType.currentIndex = card.type === "cloze" ? 1 : 0
        editTags.text = card.tags
        editCardDialog.open()
    }

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
            if (event.modifiers & Qt.ControlModifier) {
                if (event.key === Qt.Key_Z) {
                    if (event.modifiers & Qt.ShiftModifier)
                        backend.redo()
                    else
                        backend.undo()
                    event.accepted = true
                } else if (event.key === Qt.Key_Y) {
                    backend.redo()
                    event.accepted = true
                }
                return
            }
            if (newDeckDialog.opened || newCardDialog.opened || importDialog.opened
                    || editCardDialog.opened || deleteDeckDialog.opened) {
                if (event.key === Qt.Key_Escape) {
                    newDeckDialog.close();
                    newCardDialog.close();
                    importDialog.close();
                    editCardDialog.close();
                    deleteDeckDialog.close();
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
            case Qt.Key_B:
                if (browserDrawer.opened) {
                    browserDrawer.close();
                    keyHandler.forceActiveFocus();
                } else {
                    browserDrawer.open();
                    browserSearch.forceActiveFocus();
                }
                event.accepted = true;
                break;
            case Qt.Key_S:
                openStats();
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
                statsVisible = false;
                if (browserDrawer.opened)
                    browserDrawer.close()
                keyHandler.forceActiveFocus();
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
                    ToolButton {
                        text: "↶"
                        enabled: backend.canUndo
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Undo (Ctrl+Z)")
                        onClicked: { backend.undo(); keyHandler.forceActiveFocus() }
                    }
                    ToolButton {
                        text: "↷"
                        enabled: backend.canRedo
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Redo (Ctrl+Shift+Z)")
                        onClicked: { backend.redo(); keyHandler.forceActiveFocus() }
                    }
                    Button {
                        text: qsTr("Cards (B)")
                        onClicked: { browserDrawer.open(); browserSearch.forceActiveFocus() }
                    }
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
                        [qsTr("Browse / search"), "B"],
                        [qsTr("Stats"), "S"],
                        [qsTr("Undo / redo"), "Ctrl+Z / Ctrl+Shift+Z"],
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

    Popup {
        id: statsPopup
        parent: win.contentItem
        visible: statsVisible
        anchors.centerIn: parent
        width: Math.min(420, win.width - 40)
        height: 330
        modal: true
        padding: 24
        onClosed: { statsVisible = false; keyHandler.forceActiveFocus() }
        background: Rectangle {
            color: "#17171b"
            radius: Math.min(8, theme.radius)
            border.color: "#33333d"
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 12
            Label {
                text: qsTr("Stats")
                font.pixelSize: 22
                font.bold: true
            }
            Repeater {
                model: [
                    [qsTr("Reviews today"), (statsData.today || 0)],
                    [qsTr("Accuracy today"), (statsData.accuracy || 0) + "%"],
                    [qsTr("Streak"), qsTr("%1 days").arg((statsData.streakDays || 0))],
                    [qsTr("Total reviews"), (statsData.totalReviews || 0)],
                    [qsTr("Due now"), (statsData.due || 0)],
                    [qsTr("Cards"), (statsData.cards || 0)],
                    [qsTr("Decks"), (statsData.decks || 0)],
                    [qsTr("This session"), qsTr("%1 reviews · %2 correct").arg((statsData.sessionReviewed || 0)).arg((statsData.sessionCorrect || 0))]
                ]
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: modelData[0]; color: "#a8a8b0"; Layout.fillWidth: true }
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
        width: Math.min(560, win.width - 40)
        onAccepted: {
            backend.addCardDetails(newQuestion.text, newAnswer.text,
                                   newType.currentIndex === 1 ? "cloze" : "basic", newTags.text);
            newQuestion.clear();
            newAnswer.clear();
            newTags.clear();
            newType.currentIndex = 0;
            keyHandler.forceActiveFocus();
        }
        ColumnLayout {
            anchors.fill: parent
            spacing: 8
            TextArea {
                id: newQuestion
                Layout.fillWidth: true
                Layout.preferredHeight: 84
                placeholderText: qsTr("Question (LaTeX with \\( ... \\))")
                wrapMode: TextArea.Wrap
            }
            TextArea {
                id: newAnswer
                Layout.fillWidth: true
                Layout.preferredHeight: 84
                placeholderText: qsTr("Answer (cloze cards: write the sentence in the question)")
                wrapMode: TextArea.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                ComboBox {
                    id: newType
                    model: [qsTr("Basic"), qsTr("Cloze")]
                }
                TextField {
                    id: newTags
                    Layout.fillWidth: true
                    placeholderText: qsTr("Tags, comma separated")
                }
            }
        }
    }

    Dialog {
        id: editCardDialog
        title: qsTr("Edit card")
        standardButtons: Dialog.Ok | Dialog.Cancel
        modal: true
        anchors.centerIn: parent
        width: Math.min(560, win.width - 40)
        onAccepted: {
            backend.updateCard(editCardId, editQuestion.text, editAnswer.text,
                               editType.currentIndex === 1 ? "cloze" : "basic", editTags.text);
            keyHandler.forceActiveFocus();
        }
        onClosed: keyHandler.forceActiveFocus()
        ColumnLayout {
            anchors.fill: parent
            spacing: 8
            TextArea {
                id: editQuestion
                Layout.fillWidth: true
                Layout.preferredHeight: 84
                wrapMode: TextArea.Wrap
            }
            TextArea {
                id: editAnswer
                Layout.fillWidth: true
                Layout.preferredHeight: 84
                wrapMode: TextArea.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                ComboBox {
                    id: editType
                    model: [qsTr("Basic"), qsTr("Cloze")]
                }
                TextField {
                    id: editTags
                    Layout.fillWidth: true
                    placeholderText: qsTr("Tags, comma separated")
                }
            }
        }
    }

    Dialog {
        id: deleteDeckDialog
        title: qsTr("Delete deck?")
        standardButtons: Dialog.Ok | Dialog.Cancel
        modal: true
        anchors.centerIn: parent
        width: Math.min(440, win.width - 40)
        onAccepted: {
            backend.deleteCurrentDeck();
            keyHandler.forceActiveFocus();
        }
        onClosed: keyHandler.forceActiveFocus()
        Label {
            text: qsTr("Remove “%1” and all of its cards? Ctrl+Z can undo this.").arg(backend.currentDeck)
            wrapMode: Text.WordWrap
            anchors.fill: parent
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

    Drawer {
        id: browserDrawer
        edge: Qt.RightEdge
        width: Math.min(400, win.width - 40)
        height: win.height
        modal: true
        interactive: true
        background: Rectangle {
            color: "#17171b"
            border.color: "#2a2a32"
        }
        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: qsTr("Cards")
                    font.pixelSize: 20
                    font.bold: true
                    Layout.fillWidth: true
                }
                Label {
                    text: backend.cardList.length
                    color: theme.accent
                }
                ToolButton {
                    text: "×"
                    onClicked: {
                        browserDrawer.close()
                        keyHandler.forceActiveFocus()
                    }
                }
            }

            TextField {
                id: browserSearch
                Layout.fillWidth: true
                placeholderText: qsTr("Search questions, answers and tags")
                text: backend.cardFilter
                onTextChanged: backend.cardFilter = text
                onAccepted: keyHandler.forceActiveFocus()
            }

            Label {
                Layout.fillWidth: true
                visible: backend.cardList.length === 0
                text: backend.cardFilter.length > 0
                      ? qsTr("No cards match “%1”.").arg(backend.cardFilter)
                      : qsTr("No cards yet. Press N to add one.")
                color: "#8e8e98"
                wrapMode: Text.WordWrap
            }

            ListView {
                id: browserList
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6
                model: backend.cardList

                delegate: Rectangle {
                    required property var modelData
                    width: ListView.view.width
                    height: cardRow.implicitHeight + 16
                    color: hover.containsMouse ? "#24242c" : "#1d1d23"
                    radius: Math.min(6, theme.radius)

                    RowLayout {
                        id: cardRow
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 4

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                Layout.fillWidth: true
                                text: modelData.question
                                color: "#e8e8ec"
                                wrapMode: Text.Wrap
                                maximumLineCount: 2
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: (modelData.tags.length > 0 ? modelData.tags + " · " : "") + modelData.dueText
                                color: modelData.isDue ? theme.accent : "#8e8e98"
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        ToolButton {
                            text: qsTr("Edit")
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Edit card")
                            onClicked: win.openEditor(modelData)
                        }
                        ToolButton {
                            text: qsTr("Del")
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Delete card")
                            onClicked: backend.removeCard(modelData.id)
                        }
                    }

                    HoverHandler {
                        id: hover
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Button {
                    text: qsTr("Delete deck")
                    onClicked: deleteDeckDialog.open()
                }
                Item { Layout.fillWidth: true }
                Label {
                    text: qsTr("Ctrl+Z undo")
                    color: "#8e8e98"
                }
            }
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
