import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Main.qml map. Names, not line numbers, so this stays true after edits.
// keyHandler - one Keys.onPressed for every screen
// deck selector - shared trigger and Candidate deck popup on Home and Study
// study - study Panel with the Flashcard viewport, edge fades, end hint and reveal MouseArea
// library - deckList and cardView ListViews
// settings - ScrollView
// help overlay - keyboard Repeater

ApplicationWindow {
    id: win
    width: 960
    height: 680
    minimumWidth: 760
    minimumHeight: 540
    visible: true
    title: qsTr("omacards")
    color: theme.background

    Material.theme: theme.dark ? Material.Dark : Material.Light
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    readonly property color text: theme.foreground
    readonly property color soft: theme.soft
    readonly property color muted: theme.muted
    readonly property color panel: theme.panel
    readonly property color raised: theme.raised
    readonly property color border: theme.border
    readonly property color accentInk: theme.accentForeground
    readonly property var screenOrder: ["home", "study", "library", "settings"]
    readonly property int screenIndex: Math.max(0, screenOrder.indexOf(screen))

    property string screen: "home"
    property bool helpVisible: false
    property bool statsVisible: false
    property var statsData: ({})
    property string editCardId: ""
    property string pendingDeck: ""
    property string noticeMessage: ""
    property int cardIndex: 0
    property string lastPresentedStudyState: ""
    readonly property bool cramDistributionVisible: backend.mode === "cram" && cramQueueHasCards()

    // Each arriving math result notifies both rich-text bindings. Keep the
    // scaled markup so an unchanged html/width pair is scanned only once.
    property var imageScaleCache: ({})

    function withAlpha(color, alpha) {
        return Qt.rgba(color.r, color.g, color.b, alpha)
    }

    function clampedRadius(cap) {
        return Math.min(cap, theme.radius)
    }

    function clampCardIndex() {
        const count = backend.cardList.length
        cardIndex = count === 0 ? 0 : Math.min(Math.max(cardIndex, 0), count - 1)
    }

    function cramQueueHasCards() {
        const counts = backend.boxDistribution
        for (let i = 0; i < counts.length; ++i)
            if (counts[i] > 0)
                return true
        return false
    }

    function closeDeckPicker() {
        if (homePicker && homePicker.open)
            homePicker.close()
        if (studyPicker && studyPicker.open)
            studyPicker.close()
    }

    function startStudy(mode) {
        closeDeckPicker()
        backend.setMode(mode)
        backend.refresh()
        screen = "study"
        keyHandler.forceActiveFocus()
    }

    // R and the Study Review button share this path. With an empty Review
    // queue the session adds session-only New cards (or presents a waiting
    // repeat); everywhere else it keeps the old start-study behaviour.
    function reviewAction() {
        closeDeckPicker()
        if (backend.mode === "review" && !backend.hasCard) {
            backend.reviewWhenEmpty(5)
            keyHandler.forceActiveFocus()
            return
        }
        startStudy("review")
    }

    function goHome() {
        closeDeckPicker()
        screen = "home"
        helpVisible = false
        statsVisible = false
        keyHandler.forceActiveFocus()
    }

    function openLibrary() {
        closeDeckPicker()
        backend.refresh()
        screen = "library"
        clampCardIndex()
        keyHandler.forceActiveFocus()
    }

    function moveCardSelection(delta) {
        const count = backend.cardList.length
        if (count === 0)
            return
        let next = cardIndex + delta
        if (next < 0) next = count - 1
        if (next >= count) next = 0
        cardIndex = next
        cardView.positionViewAtIndex(next, ListView.Contain)
    }

    function selectedCard() {
        const list = backend.cardList
        if (list.length === 0 || cardIndex < 0 || cardIndex >= list.length)
            return null
        return list[cardIndex]
    }

    function editSelectedCard() {
        const card = selectedCard()
        if (card) openEditor(card)
    }

    function removeSelectedCard() {
        const card = selectedCard()
        if (card) backend.removeCard(card.id)
    }

    function openSettings() {
        closeDeckPicker()
        screen = "settings"
        keyHandler.forceActiveFocus()
    }

    function openStats() {
        statsData = backend.statsSummary()
        statsVisible = true
    }

    // Rich text sets <img> sizes from width/height attributes. Bring wide
    // pictures down to the text column width without touching the text around
    // them or the renderer that produced them.
    function scaleImagesToWidth(html, maxWidth) {
        if (!html || maxWidth <= 0)
            return html || ""
        const cache = win.imageScaleCache
        const key = maxWidth + "\u0000" + html
        if (Object.prototype.hasOwnProperty.call(cache, key))
            return cache[key]
        const imageTags = /<img\b[^>]*>/gi
        const scaled = html.replace(imageTags, function(tag) {
            const widthMatch = /\bwidth\s*=\s*["']?([0-9]*\.?[0-9]+)["']?/i.exec(tag)
            const heightMatch = /\bheight\s*=\s*["']?([0-9]*\.?[0-9]+)["']?/i.exec(tag)
            const srcMatch = /\bsrc\s*=\s*["']([^"']+)["']/i.exec(tag)
            let naturalWidth = widthMatch ? parseFloat(widthMatch[1]) : 0
            let naturalHeight = heightMatch ? parseFloat(heightMatch[1]) : 0

            if (!(naturalWidth > 0 && naturalHeight > 0)) {
                if (!srcMatch)
                    return tag
                const source = srcMatch[1]
                const url = /^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(source) ? source : Qt.resolvedUrl(source)
                const probe = Qt.createQmlObject(
                            'import QtQuick 2.15; Image { asynchronous: false; source: ' + JSON.stringify(url) + ' }',
                            win.contentItem)
                if (!probe || probe.status !== Image.Ready) {
                    if (probe)
                        probe.destroy()
                    return tag
                }
                naturalWidth = probe.implicitWidth
                naturalHeight = probe.implicitHeight
                probe.destroy()
            }

            if (!(naturalWidth > 0 && naturalHeight > 0) || naturalWidth <= maxWidth)
                return tag

            const targetWidth = Math.round(maxWidth)
            const targetHeight = Math.max(1, Math.round(naturalHeight * targetWidth / naturalWidth))
            const stripped = tag.replace(/\s+(?:width|height)\s*=\s*(?:"[^"]*"|'[^']*'|[^\s>]+)/gi, "")
            const closing = stripped.slice(-2) === "/>" ? "/>" : ">"
            const body = stripped.slice(0, stripped.length - closing.length)
            const size = " width=\"" + targetWidth + "\" height=\"" + targetHeight + "\""
            return closing === "/>" ? body + size + " />" : body + size + ">"
        })
        const keys = Object.keys(cache)
        if (keys.length >= 32)
            delete cache[keys[0]]
        cache[key] = scaled
        return scaled
    }

    function resetStudyScroll() {
        const flick = flashcardViewport.contentItem
        if (flick) {
            flick.contentX = 0
            flick.contentY = 0
        }
    }

    function revealAndResetFlashcardViewport() {
        backend.reveal()
        resetStudyScroll()
    }

    function scrollStudy(direction) {
        const flick = flashcardViewport.contentItem
        if (!flick || !flashcardViewport.visible)
            return false
        const maxY = Math.max(0, flick.contentHeight - flick.height)
        const target = Math.min(maxY, Math.max(0, flick.contentY + direction * 48))
        if (Math.abs(target - flick.contentY) < 1)
            return false
        flick.contentY = target
        return true
    }

    Connections {
        target: backend
        function onCardChanged() {
            const key = backend.currentCardId + ":" + backend.showingAnswer
            if (key !== win.lastPresentedStudyState) {
                win.lastPresentedStudyState = key
                win.resetStudyScroll()
            }
        }
        function onCardsChanged() {
            win.clampCardIndex()
        }
    }

    function openEditor(card) {
        editCardId = card.id
        editQuestion.text = card.question
        editAnswer.text = card.answer
        editType.currentIndex = card.type === "cloze" ? 1 : 0
        editTags.text = card.tags
        editCardDialog.open()
    }

    function heroTitle() {
        if (backend.totalCards === 0)
            return qsTr("Start your first deck.")
        if (backend.dueCount > 0)
            return qsTr("Time to review.")
        if (backend.newCount > 0)
            return qsTr("New cards are waiting.")
        return qsTr("All caught up.")
    }

    function heroDetail() {
        if (backend.totalCards === 0)
            return qsTr("Decks are plain text files, so you or your agent can edit them directly.")
        if (backend.deckDue > 0)
            return qsTr("%1 due in %2. One card at a time, Again or Good.")
                .arg(backend.deckDue).arg(backend.currentDeck)
        if (backend.deckNew === 1)
            return qsTr("One new card left today in %1.").arg(backend.currentDeck)
        if (backend.deckNew > 1)
            return qsTr("%1 new cards left today in %2.", "", backend.deckNew)
                .arg(backend.deckNew).arg(backend.currentDeck)
        if (backend.dueCount > 0)
            return qsTr("%1 cards are due in other decks. Use the deck selector to switch.")
                .arg(backend.dueCount)
        return qsTr("Nothing is due in %1. Cram a deck or import new material.").arg(backend.currentDeck)
    }

    // Both pickers read the same ordered summaries. Index lookup lives here
    // so the Current deck, the delegate and the bracket keys cannot disagree.
    function deckIndex(name) {
        const entries = backend.deckSelector
        for (let i = 0; i < entries.length; ++i) {
            if (entries[i].name === name)
                return i
        }
        return -1
    }

    function deckSummary(entry) {
        if (!entry)
            return qsTr("No deck")
        return qsTr("%1 · %2 cards · %3 due")
            .arg(entry.name).arg(entry.cards).arg(entry.due)
    }

    component ActionButton: Rectangle {
        id: control
        property string label
        property string hint
        property bool primary: false
        property bool danger: false
        property bool compact: false
        signal clicked()

        implicitWidth: buttonRow.implicitWidth + (compact ? 22 : 40)
        implicitHeight: compact ? 32 : 46
        radius: win.clampedRadius(10)
        color: primary ? theme.accent : (hover.hovered ? raised : panel)
        border.width: primary ? 0 : 1
        border.color: danger ? withAlpha(theme.red, 0.6) : win.border
        opacity: enabled ? 1 : 0.4
        Behavior on color { ColorAnimation { duration: 90 } }

        RowLayout {
            id: buttonRow
            anchors.centerIn: parent
            spacing: 9

            Label {
                text: control.label
                color: control.primary ? win.accentInk : control.danger ? theme.red : win.text
                font.pixelSize: control.compact ? 12 : 15
                font.weight: Font.DemiBold
            }

            Rectangle {
                visible: control.hint.length > 0
                implicitWidth: hintLabel.implicitWidth + 12
                implicitHeight: 20
                radius: 4
                color: control.primary ? win.withAlpha(win.accentInk, 0.14) : raised

                Label {
                    id: hintLabel
                    anchors.centerIn: parent
                    text: control.hint
                    color: control.primary ? win.withAlpha(win.accentInk, 0.78) : win.muted
                    font.pixelSize: 11
                    font.family: "monospace"
                }
            }
        }

        HoverHandler { id: hover }

        MouseArea {
            anchors.fill: parent
            enabled: control.enabled
            cursorShape: Qt.PointingHandCursor
            onClicked: control.clicked()
        }
    }

    // Candidate deck selector. The trigger and rows are app-styled Rectangles;
    // opening never switches decks, and keys outside the state machine are
    // swallowed so no screen shortcut fires behind the popup.
    component DeckSelector: Rectangle {
        id: picker
        property string triggerObjectName: ""
        property string summaryObjectName: ""
        property string popupObjectName: ""
        property int pickerWidth: 320
        property bool open: false
        property string candidateName: backend.currentDeck
        property int naturalWidth: pickerWidth
        // The popup starts at the trigger width, grows for long names and
        // stops at 65% of the window so it still fits at minimum size.
        property int popupWidth: Math.min(naturalWidth, Math.floor(win.width * 0.65))

        FontMetrics {
            id: nameMetrics
            font.pixelSize: 14
            font.weight: Font.DemiBold
        }

        objectName: picker.triggerObjectName
        width: pickerWidth
        implicitWidth: pickerWidth
        height: 42
        implicitHeight: 42
        Layout.preferredWidth: pickerWidth
        radius: win.clampedRadius(7)
        color: listPopup.opened ? win.raised : win.panel
        border.width: 1
        border.color: listPopup.opened ? win.withAlpha(theme.accent, 0.6) : win.border

        function measurePopupWidth() {
            let widest = 0
            const entries = backend.deckSelector
            for (let i = 0; i < entries.length; ++i)
                widest = Math.max(widest, nameMetrics.advanceWidth(entries[i].name))
            // Row margins and spacing, the Current tag and the popup padding.
            naturalWidth = Math.ceil(Math.max(pickerWidth, widest + 112))
        }

        function openAtCurrent() {
            picker.candidateName = backend.currentDeck
            picker.measurePopupWidth()
            picker.open = true
            listPopup.open()
            const at = win.deckIndex(picker.candidateName)
            if (at >= 0)
                deckList.positionViewAtIndex(at, ListView.Contain)
        }

        function close() {
            picker.open = false
            picker.candidateName = backend.currentDeck
            listPopup.close()
        }

        function moveCandidate(delta) {
            const entries = backend.deckSelector
            if (!entries.length)
                return
            const at = win.deckIndex(picker.candidateName)
            const next = (at + delta + entries.length) % entries.length
            picker.candidateName = entries[next].name
            deckList.positionViewAtIndex(next, ListView.Contain)
        }

        function commit(name) {
            if (!name)
                return
            if (name !== backend.currentDeck)
                backend.selectDeck(name)
            win.closeDeckPicker()
            keyHandler.forceActiveFocus()
        }

        function handleKey(event) {
            switch (event.key) {
            case Qt.Key_BracketLeft:
                picker.moveCandidate(-1)
                break
            case Qt.Key_BracketRight:
                picker.moveCandidate(1)
                break
            case Qt.Key_Return:
            case Qt.Key_Enter:
                picker.commit(picker.candidateName)
                break
            case Qt.Key_Escape:
                win.closeDeckPicker()
                break
            }
            event.accepted = true
        }

        Connections {
            target: backend
            function onCurrentDeckChanged() {
                if (!picker.open)
                    picker.candidateName = backend.currentDeck
            }
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 12
            spacing: 8

            Label {
                objectName: picker.summaryObjectName
                Layout.fillWidth: true
                text: win.deckSummary(backend.deckSelector[win.deckIndex(backend.currentDeck)])
                color: win.text
                font.pixelSize: 13
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                ToolTip.text: text
                ToolTip.visible: triggerMouse.containsMouse && truncated
                ToolTip.delay: 0
            }

            Label {
                objectName: picker.triggerObjectName + "Chevron"
                text: listPopup.opened ? "▴" : "▾"
                color: win.muted
                font.pixelSize: 10
            }

            Label {
                objectName: picker.triggerObjectName + "Hint"
                text: "[ / ]"
                color: win.muted
                font.family: "monospace"
                font.pixelSize: 11
            }
        }

        MouseArea {
            id: triggerMouse
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: picker.openAtCurrent()
        }

        Popup {
            id: listPopup
            objectName: picker.popupObjectName
            z: 2
            x: 0
            y: picker.height + 4
            width: popupWidth
            height: 8 + deckList.height + 6 + footer.implicitHeight + 8
            padding: 0
            topPadding: 0
            bottomPadding: 0
            leftPadding: 0
            rightPadding: 0
            modal: false
            focus: false
            popupType: Popup.Item
            closePolicy: Popup.NoAutoClose
            background: Rectangle {
                color: theme.background
                radius: win.clampedRadius(10)
                border.width: 1
                border.color: win.border
            }

            onClosed: win.closeDeckPicker()

            ListView {
                id: deckList
                objectName: picker.popupObjectName + "List"
                x: 8
                y: 8
                width: popupWidth - 16
                height: 264 // five 48px rows plus four 6px gaps
                clip: true
                model: backend.deckSelector
                spacing: 6
                currentIndex: -1

                function scrollToRow(name) {
                    const at = win.deckIndex(name)
                    if (at >= 0)
                        positionViewAtIndex(at, ListView.Center)
                }

                delegate: Rectangle {
                    id: deckRow
                    objectName: picker.popupObjectName + "Row" + index
                    property bool candidate: modelData.name === picker.candidateName
                    property bool current: modelData.name === backend.currentDeck
                    width: deckList.width
                    height: 48
                    radius: win.clampedRadius(7)
                    color: deckRow.candidate ? theme.accent : rowMouse.containsMouse ? win.raised : win.panel
                    border.width: deckRow.candidate ? 0 : 1
                    border.color: win.border

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2

                            Label {
                                id: nameLabel
                                Layout.fillWidth: true
                                text: modelData.name
                                color: deckRow.candidate ? win.accentInk : win.text
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                                ToolTip.text: modelData.name
                                ToolTip.visible: rowMouse.containsMouse && truncated
                                ToolTip.delay: 0
                            }

                            Label {
                                objectName: picker.popupObjectName + "RowCounts" + index
                                Layout.fillWidth: true
                                text: qsTr("%1 cards · %2 due · %3 new")
                                      .arg(modelData.cards).arg(modelData.due).arg(modelData["new"])
                                color: deckRow.candidate ? win.withAlpha(win.accentInk, 0.75) : win.muted
                                font.pixelSize: 11
                            }
                        }

                        Rectangle {
                            objectName: picker.popupObjectName + "RowTag" + index
                            property string tagText: tagLabel.text
                            visible: deckRow.current
                            implicitWidth: tagLabel.implicitWidth + 14
                            implicitHeight: 20
                            radius: 10
                            border.width: 1
                            border.color: deckRow.candidate ? win.withAlpha(win.accentInk, 0.6) : win.muted

                            Label {
                                id: tagLabel
                                anchors.centerIn: parent
                                text: qsTr("Current")
                                color: deckRow.candidate ? win.withAlpha(win.accentInk, 0.8) : win.muted
                                font.pixelSize: 10
                            }
                        }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: picker.commit(modelData.name)
                    }
                }
            }

            Label {
                id: footer
                objectName: picker.popupObjectName + "Footer"
                x: 8
                y: 8 + deckList.height + 6
                width: popupWidth - 16
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("[ / ] move · Enter switch · Esc cancel")
                color: win.muted
                font.pixelSize: 11
            }
        }
    }

    component LinkButton: Label {
        signal clicked()
        color: hover.hovered ? theme.accent : muted
        font.pixelSize: 12
        font.family: "monospace"

        HoverHandler { id: hover }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: parent.clicked()
        }
    }

    component Stat: ColumnLayout {
        property string value
        property string label
        property color valueColor: win.text

        spacing: 2
        Label {
            text: parent.value
            color: parent.valueColor
            font.pixelSize: 30
            font.weight: Font.DemiBold
        }
        Label {
            text: parent.label
            color: win.muted
            font.pixelSize: 12
        }
    }

    component Panel: Rectangle {
        color: win.panel
        radius: win.clampedRadius(12)
        border.width: 1
        border.color: win.border
    }

    component InputField: TextField {
        color: win.text
        placeholderTextColor: win.muted
        selectionColor: win.withAlpha(theme.accent, 0.35)
        selectedTextColor: win.text
        font.pixelSize: 14
        leftPadding: 12
        rightPadding: 12
        background: Rectangle {
            color: win.raised
            radius: win.clampedRadius(7)
            border.width: 1
            border.color: parent.activeFocus ? win.withAlpha(theme.accent, 0.7) : win.border
        }
    }

    component TextBox: TextArea {
        color: win.text
        placeholderTextColor: win.muted
        selectionColor: win.withAlpha(theme.accent, 0.35)
        selectedTextColor: win.text
        font.pixelSize: 14
        leftPadding: 12
        rightPadding: 12
        topPadding: 10
        bottomPadding: 10
        wrapMode: TextArea.Wrap
        background: Rectangle {
            color: win.raised
            radius: win.clampedRadius(7)
            border.width: 1
            border.color: parent.activeFocus ? win.withAlpha(theme.accent, 0.7) : win.border
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: (homePicker && homePicker.open) || (studyPicker && studyPicker.open)
        z: 1
        onClicked: win.closeDeckPicker()
    }

    Item {
        anchors.fill: parent

        Rectangle {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 3
            color: theme.accent
        }

        Item {
            id: keyHandler
            objectName: "keyHandler"
            anchors.fill: parent
            anchors.topMargin: 3
            focus: true
            LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
            LayoutMirroring.childrenInherit: true

            Keys.onPressed: function(event) {
                // An open Candidate selector owns every key, before any Study
                // scroll, rating, shortcut or dialog branch can see it.
                if (studyPicker && studyPicker.open) {
                    studyPicker.handleKey(event)
                    return
                }

                // Dialogs, the Deck selector and the overlay panels own every
                // key while open. This guard must stay above undo/redo so a
                // shortcut behind an owner cannot edit the Deck.
                if (newDeckDialog.opened || newCardDialog.opened || importDialog.opened
                        || editCardDialog.opened || deleteDeckDialog.opened) {
                    if (event.key === Qt.Key_Escape) {
                        newDeckDialog.close()
                        newCardDialog.close()
                        importDialog.close()
                        editCardDialog.close()
                        deleteDeckDialog.close()
                        keyHandler.forceActiveFocus()
                        event.accepted = true
                    }
                    return
                }

                if (homePicker.open) {
                    homePicker.handleKey(event)
                    return
                }

                if (helpVisible || statsVisible) {
                    if (event.key === Qt.Key_Escape || event.key === Qt.Key_Question
                            || event.key === Qt.Key_F1 || event.key === Qt.Key_S) {
                        helpVisible = false
                        statsVisible = false
                        event.accepted = true
                    }
                    return
                }

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

                switch (event.key) {
                case Qt.Key_Q:
                    Qt.quit()
                    event.accepted = true
                    return
                case Qt.Key_Question:
                case Qt.Key_F1:
                    helpVisible = true
                    event.accepted = true
                    return
                case Qt.Key_S:
                    openStats()
                    event.accepted = true
                    return
                case Qt.Key_Comma:
                    if (screen === "settings") goHome()
                    else openSettings()
                    event.accepted = true
                    return
                case Qt.Key_B:
                    if (screen === "library") goHome()
                    else openLibrary()
                    event.accepted = true
                    return
                case Qt.Key_N:
                    newCardDialog.open()
                    event.accepted = true
                    return
                case Qt.Key_D:
                    newDeckDialog.open()
                    event.accepted = true
                    return
                case Qt.Key_I:
                    importDialog.open()
                    event.accepted = true
                    return
                case Qt.Key_E:
                    backend.editDeckFile()
                    event.accepted = true
                    return
                case Qt.Key_Escape:
                    if (screen === "study") {
                        goHome()
                    } else if (screen === "library" || screen === "settings") {
                        goHome()
                    }
                    event.accepted = true
                    return
                }

                if (screen === "home") {
                    switch (event.key) {
                    case Qt.Key_BracketLeft:
                    case Qt.Key_BracketRight:
                        homePicker.openAtCurrent()
                        event.accepted = true
                        break
                    case Qt.Key_Space:
                    case Qt.Key_Return:
                    case Qt.Key_Enter:
                    case Qt.Key_R:
                        startStudy("review")
                        event.accepted = true
                        break
                    case Qt.Key_C:
                        startStudy("cram")
                        event.accepted = true
                        break
                    }
                    return
                }

                if (screen === "study") {
                    switch (event.key) {
                    case Qt.Key_Space:
                    case Qt.Key_Return:
                    case Qt.Key_Enter:
                        win.revealAndResetFlashcardViewport()
                        event.accepted = true
                        break
                    case Qt.Key_Up:
                        if (!win.scrollStudy(-1))
                            win.revealAndResetFlashcardViewport()
                        event.accepted = true
                        break
                    case Qt.Key_Down:
                        if (!win.scrollStudy(1))
                            win.revealAndResetFlashcardViewport()
                        event.accepted = true
                        break
                    case Qt.Key_K:
                        win.scrollStudy(-1)
                        event.accepted = true
                        break
                    case Qt.Key_J:
                        win.scrollStudy(1)
                        event.accepted = true
                        break
                    case Qt.Key_1:
                    case Qt.Key_A:
                    case Qt.Key_Left:
                        backend.showingAnswer ? backend.rate(1) : win.revealAndResetFlashcardViewport()
                        event.accepted = true
                        break
                    case Qt.Key_2:
                    case Qt.Key_G:
                    case Qt.Key_Right:
                        backend.showingAnswer ? backend.rate(3) : win.revealAndResetFlashcardViewport()
                        event.accepted = true
                        break
                    case Qt.Key_R:
                        reviewAction()
                        event.accepted = true
                        break
                    case Qt.Key_C:
                        startStudy("cram")
                        event.accepted = true
                        break
                    case Qt.Key_BracketLeft:
                    case Qt.Key_BracketRight:
                        studyPicker.openAtCurrent()
                        event.accepted = true
                        break
                    }
                }

                if (screen === "library") {
                    switch (event.key) {
                    case Qt.Key_J:
                    case Qt.Key_Down:
                        win.moveCardSelection(1)
                        event.accepted = true
                        break
                    case Qt.Key_K:
                    case Qt.Key_Up:
                        win.moveCardSelection(-1)
                        event.accepted = true
                        break
                    case Qt.Key_Return:
                    case Qt.Key_Enter:
                        win.editSelectedCard()
                        event.accepted = true
                        break
                    case Qt.Key_Delete:
                    case Qt.Key_Backspace:
                        win.removeSelectedCard()
                        event.accepted = true
                        break
                    case Qt.Key_Slash:
                        librarySearch.forceActiveFocus()
                        event.accepted = true
                        break
                    }
                }
            }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 28
                spacing: 16

                StackLayout {
                    objectName: "screenStack"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    currentIndex: win.screenIndex

                    // ---------------------------------------------------- home
                    Item {
                        ColumnLayout {
                            anchors.centerIn: parent
                            width: Math.min(parent.width, 660)
                            spacing: 24

                            RowLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 12

                                Rectangle {
                                    width: 22
                                    height: 22
                                    radius: win.clampedRadius(5)
                                    color: theme.accent
                                }

                                Label {
                                    text: "omacards"
                                    color: win.text
                                    font.pixelSize: 22
                                    font.weight: Font.DemiBold
                                }

                                Label {
                                    text: qsTr("flashcards for Omarchy")
                                    color: win.muted
                                    font.pixelSize: 14
                                    Layout.alignment: Qt.AlignBaseline
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: win.heroTitle()
                                color: win.text
                                font.pixelSize: 38
                                font.weight: Font.DemiBold
                                wrapMode: Text.Wrap
                            }

                            Label {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: win.heroDetail()
                                color: win.soft
                                font.pixelSize: 16
                                wrapMode: Text.Wrap
                            }

                            Panel {
                                Layout.fillWidth: true
                                implicitHeight: 112

                                RowLayout {
                                    anchors.fill: parent
                                    anchors.margins: 22
                                    spacing: 20

                                    Stat {
                                        Layout.fillWidth: true
                                        value: backend.dueCount
                                        label: qsTr("Due now")
                                        valueColor: theme.accent
                                    }

                                    Rectangle { width: 1; Layout.fillHeight: true; color: win.border }

                                    Stat {
                                        Layout.fillWidth: true
                                        value: backend.newCount
                                        label: qsTr("New left today")
                                        valueColor: theme.green
                                    }

                                    Rectangle { width: 1; Layout.fillHeight: true; color: win.border }

                                    Stat {
                                        Layout.fillWidth: true
                                        value: backend.streak
                                        label: qsTr("Day streak")
                                        valueColor: theme.yellow
                                    }
                                }
                            }

                            RowLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 10

                                Label {
                                    text: qsTr("Deck")
                                    color: win.muted
                                    font.pixelSize: 13
                                }

                                DeckSelector {
                                    id: homePicker
                                    triggerObjectName: "homeDeckSelector"
                                    summaryObjectName: "homeDeckLabel"
                                    popupObjectName: "homeDeckPopup"
                                    pickerWidth: 320
                                }
                            }

                            RowLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 10

                                ActionButton {
                                    primary: true
                                    label: backend.dueCount > 0 || backend.newCount > 0
                                           ? qsTr("Start review") : qsTr("Review anyway")
                                    hint: "R"
                                    onClicked: win.startStudy("review")
                                }

                                ActionButton {
                                    label: qsTr("Cram")
                                    hint: "C"
                                    onClicked: win.startStudy("cram")
                                }

                                ActionButton {
                                    label: qsTr("Library")
                                    hint: "B"
                                    onClicked: win.openLibrary()
                                }
                            }

                            RowLayout {
                                Layout.alignment: Qt.AlignHCenter
                                spacing: 16

                                Label {
                                    text: backend.currentDeck.length > 0
                                          ? qsTr("%1 · %2 cards").arg(backend.currentDeck).arg(backend.totalCards)
                                          : qsTr("No decks yet")
                                    color: win.muted
                                    font.pixelSize: 12
                                }

                                LinkButton {
                                    text: qsTr("Settings (,)")
                                    onClicked: win.openSettings()
                                }
                            }
                        }
                    }

                    // --------------------------------------------------- study
                    Item {
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 16

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12

                                ColumnLayout {
                                    spacing: 2
                                    DeckSelector {
                                        id: studyPicker
                                        triggerObjectName: "studyDeckSelector"
                                        summaryObjectName: "studyDeckLabel"
                                        popupObjectName: "studyDeckPopup"
                                        pickerWidth: 260
                                    }
                                    Label {
                                        objectName: "studyModeLabel"
                                        text: backend.mode === "cram"
                                              ? qsTr("Cram · endless, in-session boxes")
                                              : qsTr("Review · FSRS-5 scheduling")
                                        color: win.muted
                                        font.pixelSize: 12
                                    }
                                }

                                Item { Layout.fillWidth: true }

                                ActionButton {
                                    objectName: "studyReviewButton"
                                    compact: true
                                    label: backend.reviewButtonLabel
                                    hint: "R"
                                    primary: backend.mode === "review"
                                    onClicked: win.reviewAction()
                                }
                                ActionButton {
                                    objectName: "studyCramButton"
                                    compact: true
                                    label: qsTr("Cram")
                                    hint: "C"
                                    primary: backend.mode === "cram"
                                    onClicked: win.startStudy("cram")
                                }
                                ActionButton {
                                    objectName: "studyHomeButton"
                                    compact: true
                                    label: qsTr("Home")
                                    hint: "Esc"
                                    onClicked: win.goHome()
                                }
                            }

                            Panel {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                border.color: win.withAlpha(theme.accent, 0.32)

                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 24
                                    spacing: 14

                                    Label {
                                        Layout.alignment: Qt.AlignHCenter
                                        objectName: "cramLeitnerBoxLabel"
                                        text: {
                                            if (!backend.hasCard)
                                                return qsTr("SESSION")
                                            return backend.mode === "cram"
                                                   ? qsTr("CRAM · BOX %1 OF 5").arg(backend.currentLeitnerBox)
                                                   : qsTr("REVIEW · %1 DUE").arg(backend.dueCount)
                                        }
                                        color: win.muted
                                        font.pixelSize: 12
                                        font.letterSpacing: 1.5
                                    }

                                    Item {
                                        id: studyViewportFrame
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        visible: backend.hasCard

                                        ScrollView {
                                            id: flashcardViewport
                                            objectName: "flashcardViewport"
                                            anchors.fill: parent
                                            visible: backend.hasCard
                                            clip: true
                                            contentWidth: width
                                            contentHeight: studyText.implicitHeight
                                            ScrollBar.horizontal.objectName: "studyHorizontalScrollBar"
                                            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                                            ScrollBar.vertical.objectName: "studyVerticalScrollBar"
                                            ScrollBar.vertical.policy: ScrollBar.AsNeeded

                                            readonly property bool contentOverflows: {
                                                const flick = contentItem
                                                return !!flick && flick.contentHeight > flick.height + 1
                                            }
                                            readonly property bool canScrollUp: {
                                                const flick = contentItem
                                                return !!flick && flick.contentY > 1
                                            }
                                            readonly property bool canScrollDown: {
                                                const flick = contentItem
                                                return !!flick
                                                       && flick.contentY < flick.contentHeight - flick.height - 1
                                            }

                                            Item {
                                                width: flashcardViewport.width
                                                height: studyText.implicitHeight

                                                Column {
                                                    id: studyText
                                                    objectName: "studyTextColumn"
                                                    anchors.horizontalCenter: parent.horizontalCenter
                                                    width: Math.min(flashcardViewport.width - 96, 660)
                                                    spacing: 18

                                                    Label {
                                                        objectName: "studyQuestionLabel"
                                                        width: parent.width
                                                        horizontalAlignment: Text.AlignHCenter
                                                        wrapMode: Text.Wrap
                                                        textFormat: Text.RichText
                                                        text: win.scaleImagesToWidth(backend.question, studyText.width)
                                                        color: win.text
                                                        font.pixelSize: 30
                                                        font.weight: Font.DemiBold
                                                        onLinkActivated: function(link) { Qt.openUrlExternally(link) }
                                                    }

                                                    Rectangle {
                                                        objectName: "studySeparator"
                                                        width: parent.width
                                                        height: 1
                                                        color: win.border
                                                        visible: backend.showingAnswer
                                                    }

                                                    Label {
                                                        objectName: "studyAnswerLabel"
                                                        width: parent.width
                                                        visible: backend.showingAnswer
                                                        horizontalAlignment: Text.AlignHCenter
                                                        wrapMode: Text.Wrap
                                                        textFormat: Text.RichText
                                                        text: win.scaleImagesToWidth(backend.answer, studyText.width)
                                                        color: theme.accent
                                                        font.pixelSize: 24
                                                    }
                                                }
                                            }
                                        }

                                        Rectangle {
                                            objectName: "studyTopFade"
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.rightMargin: 14
                                            anchors.top: parent.top
                                            height: 34
                                            enabled: false
                                            visible: backend.hasCard && flashcardViewport.canScrollUp
                                            opacity: visible ? 0.9 : 0
                                            gradient: Gradient {
                                                GradientStop { position: 0.0; color: win.panel }
                                                GradientStop {
                                                    position: 1.0
                                                    color: Qt.rgba(win.panel.r, win.panel.g, win.panel.b, 0)
                                                }
                                            }
                                        }

                                        Rectangle {
                                            objectName: "studyBottomFade"
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.rightMargin: 14
                                            anchors.bottom: parent.bottom
                                            height: 34
                                            enabled: false
                                            visible: backend.hasCard && flashcardViewport.canScrollDown
                                            opacity: visible ? 0.9 : 0
                                            gradient: Gradient {
                                                GradientStop {
                                                    position: 0.0
                                                    color: Qt.rgba(win.panel.r, win.panel.g, win.panel.b, 0)
                                                }
                                                GradientStop { position: 1.0; color: win.panel }
                                            }
                                        }

                                        Label {
                                            objectName: "studyEndOfScrollHint"
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            anchors.bottom: parent.bottom
                                            anchors.bottomMargin: 10
                                            text: qsTr("At end — ↑/↓ flips card")
                                            color: win.muted
                                            font.pixelSize: 12
                                            visible: backend.hasCard && backend.showingAnswer
                                                     && flashcardViewport.contentOverflows && !flashcardViewport.canScrollDown
                                            opacity: visible ? 1 : 0
                                        }
                                    }

                                    ActionButton {
                                        objectName: "studyShowAnswer"
                                        Layout.alignment: Qt.AlignHCenter
                                        visible: backend.hasCard && !backend.showingAnswer
                                        primary: true
                                        label: qsTr("Show answer")
                                        hint: "Space"
                                        onClicked: win.revealAndResetFlashcardViewport()
                                    }

                                    Label {
                                        objectName: "studyHint"
                                        Layout.fillWidth: true
                                        visible: backend.hasCard && !backend.showingAnswer
                                        horizontalAlignment: Text.AlignHCenter
                                        text: qsTr("Click the card, or press Space.")
                                        color: win.muted
                                        font.pixelSize: 12
                                    }

                                    Item {
                                        objectName: "studyEmptyMessage"
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        visible: !backend.hasCard

                                        ColumnLayout {
                                            anchors.centerIn: parent
                                            width: Math.min(parent.width - 96, 660)
                                            spacing: 18

                                            Label {
                                                Layout.fillWidth: true
                                                horizontalAlignment: Text.AlignHCenter
                                                text: backend.waitingForRepeat ? qsTr("One more try in a moment…") : qsTr("All caught up.")
                                                color: win.text
                                                font.pixelSize: 34
                                                font.weight: Font.DemiBold
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                horizontalAlignment: Text.AlignHCenter
                                                wrapMode: Text.Wrap
                                                text: backend.waitingForRepeat
                                                      ? qsTr("A card you got wrong is coming back shortly.")
                                                      : qsTr("Nothing is waiting in this deck. Add a card, import a file, or keep the material warm with cram.")
                                                color: win.soft
                                                font.pixelSize: 15
                                            }
                                            RowLayout {
                                                Layout.alignment: Qt.AlignHCenter
                                                spacing: 10
                                                ActionButton {
                                                    primary: true
                                                    label: qsTr("Cram this deck")
                                                    hint: "C"
                                                    onClicked: win.startStudy("cram")
                                                }
                                                ActionButton {
                                                    label: qsTr("Back home")
                                                    hint: "Esc"
                                                    onClicked: win.goHome()
                                                }
                                            }
                                        }
                                    }
                                }

                                MouseArea {
                                    objectName: "studyClickArea"
                                    anchors.fill: parent
                                    enabled: backend.hasCard
                                    onClicked: win.revealAndResetFlashcardViewport()
                                    z: -1
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12

                                ActionButton {
                                    objectName: "studyAgainButton"
                                    danger: true
                                    label: qsTr("Again")
                                    hint: "1"
                                    enabled: backend.hasCard && backend.showingAnswer
                                    onClicked: { backend.rate(1); keyHandler.forceActiveFocus() }
                                }

                                ActionButton {
                                    objectName: "studyGoodButton"
                                    primary: true
                                    label: qsTr("Good")
                                    hint: "2"
                                    enabled: backend.hasCard && backend.showingAnswer
                                    onClicked: { backend.rate(3); keyHandler.forceActiveFocus() }
                                }

                                Item { Layout.fillWidth: true }

                                Label {
                                    objectName: "reviewGoodLabel"
                                    text: qsTr("%1 reviewed · %2 Good").arg(backend.reviewed).arg(backend.good)
                                    color: win.muted
                                    font.pixelSize: 12
                                }
                            }
                        }
                    }

                    // ------------------------------------------------- library
                    Item {
                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 14

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                ColumnLayout {
                                    spacing: 2
                                    Label {
                                        text: qsTr("Library")
                                        color: win.text
                                        font.pixelSize: 26
                                        font.weight: Font.DemiBold
                                    }
                                    Label {
                                        text: qsTr("%1 decks · %2 cards · plain-text files")
                                              .arg(backend.deckList.length).arg(backend.totalCards)
                                        color: win.muted
                                        font.pixelSize: 12
                                    }
                                }

                                Item { Layout.fillWidth: true }

                                ActionButton {
                                    compact: true
                                    label: qsTr("New deck")
                                    hint: "D"
                                    onClicked: newDeckDialog.open()
                                }
                                ActionButton {
                                    compact: true
                                    label: qsTr("Import")
                                    hint: "I"
                                    onClicked: importDialog.open()
                                }
                                ActionButton {
                                    compact: true
                                    label: qsTr("Home")
                                    hint: "Esc"
                                    onClicked: win.goHome()
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                spacing: 14

                                Panel {
                                    Layout.preferredWidth: 250
                                    Layout.fillHeight: true

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 8

                                        Label {
                                            text: qsTr("DECKS")
                                            color: win.muted
                                            font.pixelSize: 11
                                            font.letterSpacing: 1.2
                                        }

                                        ListView {
                                            id: deckList
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true
                                            clip: true
                                            spacing: 6
                                            model: backend.deckList

                                            delegate: Rectangle {
                                                id: deckRow
                                                required property var modelData
                                                width: ListView.view.width
                                                height: 58
                                                radius: win.clampedRadius(8)
                                                color: modelData.isCurrent
                                                       ? win.withAlpha(theme.accent, 0.14)
                                                       : hover.hovered ? win.raised : "transparent"
                                                border.width: modelData.isCurrent ? 1 : 0
                                                border.color: win.withAlpha(theme.accent, 0.5)

                                                Rectangle {
                                                    anchors.left: parent.left
                                                    anchors.top: parent.top
                                                    anchors.bottom: parent.bottom
                                                    width: 3
                                                    radius: 2
                                                    visible: modelData.isCurrent
                                                    color: theme.accent
                                                }

                                                ColumnLayout {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 14
                                                    anchors.rightMargin: 10
                                                    spacing: 2

                                                    Label {
                                                        Layout.fillWidth: true
                                                        text: modelData.name
                                                        color: win.text
                                                        font.pixelSize: 14
                                                        font.weight: Font.DemiBold
                                                        elide: Text.ElideRight
                                                    }
                                                    Label {
                                                        Layout.fillWidth: true
                                                        text: modelData.cards + (modelData.cards === 1 ? qsTr(" card") : qsTr(" cards"))
                                                              + " · " + modelData.due + qsTr(" due")
                                                              + (modelData["new"] > 0 ? " · " + modelData["new"] + qsTr(" new") : "")
                                                        color: win.muted
                                                        font.pixelSize: 11
                                                        elide: Text.ElideRight
                                                    }
                                                }

                                                HoverHandler { id: hover }

                                                MouseArea {
                                                    anchors.fill: parent
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        backend.selectDeck(deckRow.modelData.name)
                                                        win.cardIndex = 0
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }

                                Panel {
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true

                                    ColumnLayout {
                                        anchors.fill: parent
                                        anchors.margins: 16
                                        spacing: 10

                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 10

                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 2
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: backend.currentDeck.length > 0
                                                          ? backend.currentDeck : qsTr("No deck selected")
                                                    color: win.text
                                                    font.pixelSize: 20
                                                    font.weight: Font.DemiBold
                                                    elide: Text.ElideRight
                                                }
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: backend.cardList.length + (backend.cardList.length === 1 ? qsTr(" card") : qsTr(" cards"))
                                                          + " · " + backend.dueCount + qsTr(" due")
                                                    color: win.muted
                                                    font.pixelSize: 12
                                                }
                                            }

                                            ActionButton {
                                                compact: true
                                                label: qsTr("New card")
                                                hint: "N"
                                                onClicked: newCardDialog.open()
                                            }
                                            ActionButton {
                                                compact: true
                                                danger: true
                                                label: qsTr("Delete deck")
                                                onClicked: {
                                                    win.pendingDeck = backend.currentDeck
                                                    deleteDeckDialog.open()
                                                }
                                            }
                                        }

                                        InputField {
                                            id: librarySearch
                                            Layout.fillWidth: true
                                            placeholderText: qsTr("Search questions, answers and tags")
                                            text: backend.cardFilter
                                            onTextChanged: {
                                                backend.cardFilter = text
                                                win.cardIndex = 0
                                            }
                                            onAccepted: keyHandler.forceActiveFocus()
                                            Keys.onEscapePressed: {
                                                win.goHome()
                                                event.accepted = true
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            visible: backend.cardList.length === 0
                                            wrapMode: Text.WordWrap
                                            text: backend.cardFilter.length > 0
                                                  ? qsTr("No cards match “%1”.").arg(backend.cardFilter)
                                                  : qsTr("No cards yet. Press N to add one.")
                                            color: win.muted
                                            font.pixelSize: 12
                                        }

                                        ListView {
                                            id: cardView
                                            Layout.fillWidth: true
                                            Layout.fillHeight: true
                                            clip: true
                                            spacing: 6
                                            model: backend.cardList
                                            currentIndex: win.cardIndex

                                            delegate: Rectangle {
                                                id: cardRow
                                                required property var modelData
                                                required property int index
                                                width: ListView.view.width
                                                height: 52
                                                radius: win.clampedRadius(8)
                                                color: cardRow.ListView.isCurrentItem
                                                       ? win.withAlpha(theme.accent, 0.16)
                                                       : hover.hovered ? win.raised : win.withAlpha(win.raised, 0.45)
                                                border.width: cardRow.ListView.isCurrentItem ? 1 : 0
                                                border.color: win.withAlpha(theme.accent, 0.6)

                                                Rectangle {
                                                    width: 3
                                                    height: parent.height - 16
                                                    anchors.verticalCenter: parent.verticalCenter
                                                    anchors.left: parent.left
                                                    radius: 2
                                                    color: modelData.isDue ? theme.accent : win.border
                                                }

                                                RowLayout {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 14
                                                    anchors.rightMargin: 8
                                                    spacing: 10

                                                    ColumnLayout {
                                                        Layout.fillWidth: true
                                                        spacing: 2

                                                        Label {
                                                            Layout.fillWidth: true
                                                            text: modelData.question
                                                            color: win.text
                                                            font.pixelSize: 13
                                                            maximumLineCount: 1
                                                            elide: Text.ElideRight
                                                        }
                                                        Label {
                                                            Layout.fillWidth: true
                                                            text: (modelData.tags.length > 0 ? modelData.tags + " · " : "")
                                                                  + modelData.dueText
                                                            color: modelData.isDue ? theme.accent : win.muted
                                                            font.pixelSize: 11
                                                            elide: Text.ElideRight
                                                        }
                                                    }

                                                    LinkButton {
                                                        text: qsTr("Edit")
                                                        onClicked: win.openEditor(cardRow.modelData)
                                                    }
                                                    LinkButton {
                                                        text: qsTr("Delete")
                                                        color: hover.hovered ? theme.red : win.muted
                                                        onClicked: backend.removeCard(cardRow.modelData.id)
                                                    }
                                                }

                                                HoverHandler { id: hover }

                                                MouseArea {
                                                    anchors.fill: parent
                                                    z: -1
                                                    cursorShape: Qt.PointingHandCursor
                                                    onClicked: {
                                                        win.cardIndex = cardRow.index
                                                        keyHandler.forceActiveFocus()
                                                    }
                                                }
                                            }
                                        }

                                        RowLayout {
                                            Layout.fillWidth: true
                                            Label {
                                                text: qsTr("J/K move · Enter edit · Del delete · / search · Ctrl+Z undo")
                                                color: win.muted
                                                font.pixelSize: 11
                                            }
                                            Item { Layout.fillWidth: true }
                                            LinkButton {
                                                text: qsTr("Edit deck file")
                                                onClicked: backend.editDeckFile()
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // ------------------------------------------------ settings
                    Item {
                        ScrollView {
                            objectName: "settingsScrollView"
                            anchors.fill: parent
                            clip: true
                            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

                            ColumnLayout {
                                objectName: "settingsColumn"
                                width: Math.min(parent.width, 560)
                                x: Math.max(0, (parent.width - width) / 2)
                                spacing: 14

                            Label {
                                text: qsTr("Settings")
                                color: win.text
                                font.pixelSize: 26
                                font.weight: Font.DemiBold
                            }

                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                text: qsTr("Changes are saved immediately and apply from now on.")
                                color: win.muted
                                font.pixelSize: 12
                            }

                            Panel {
                                objectName: "settingsNewCardAllowancePanel"
                                Layout.fillWidth: true
                                implicitHeight: allowanceRow.implicitHeight + 32

                                RowLayout {
                                    id: allowanceRow
                                    anchors.fill: parent
                                    anchors.margins: 16
                                    spacing: 12

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        spacing: 2
                                        Label {
                                            objectName: "settingsNewCardAllowanceTitle"
                                            Layout.fillWidth: true
                                            text: qsTr("New-card allowance")
                                            color: win.text
                                            font.pixelSize: 14
                                            wrapMode: Text.Wrap
                                        }
                                        Label {
                                            objectName: "settingsNewCardAllowanceDescription"
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            text: qsTr("Keeps a large import manageable. 0 means review only.")
                                            color: win.muted
                                            font.pixelSize: 11
                                            wrapMode: Text.Wrap
                                        }
                                    }

                                    ActionButton {
                                        objectName: "settingsNewCardAllowanceMinus"
                                        compact: true
                                        label: "−"
                                        implicitWidth: 38
                                        Layout.minimumWidth: 38
                                        Layout.maximumWidth: 38
                                        onClicked: backend.setNewCardAllowance(
                                                       Math.max(backend.minNewCardAllowance,
                                                                backend.newCardAllowance - 1))
                                    }

                                    Label {
                                        objectName: "settingsNewCardAllowanceValue"
                                        text: backend.newCardAllowance
                                        color: win.text
                                        font.pixelSize: 17
                                        font.family: "monospace"
                                        Layout.preferredWidth: 34
                                        Layout.minimumWidth: 34
                                        Layout.maximumWidth: 34
                                        horizontalAlignment: Text.AlignHCenter
                                    }

                                    ActionButton {
                                        objectName: "settingsNewCardAllowancePlus"
                                        compact: true
                                        label: "+"
                                        implicitWidth: 38
                                        Layout.minimumWidth: 38
                                        Layout.maximumWidth: 38
                                        onClicked: backend.setNewCardAllowance(
                                                       Math.min(backend.maxNewCardAllowance,
                                                                backend.newCardAllowance + 1))
                                    }
                                }
                            }

                            Panel {
                                objectName: "settingsDayStartHourPanel"
                                Layout.fillWidth: true
                                implicitHeight: dayRow.implicitHeight + 32

                                RowLayout {
                                    id: dayRow
                                    anchors.fill: parent
                                    anchors.margins: 16
                                    spacing: 12

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        spacing: 2
                                        Label {
                                            objectName: "settingsDayStartHourTitle"
                                            Layout.fillWidth: true
                                            text: qsTr("Day-start hour")
                                            color: win.text
                                            font.pixelSize: 14
                                            wrapMode: Text.Wrap
                                        }
                                        Label {
                                            objectName: "settingsDayStartHourDescription"
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            text: qsTr("Cards due today unlock at this time, not at midnight.")
                                            color: win.muted
                                            font.pixelSize: 11
                                            wrapMode: Text.Wrap
                                        }
                                    }

                                    ActionButton {
                                        objectName: "settingsDayStartHourMinus"
                                        compact: true
                                        label: "−"
                                        implicitWidth: 38
                                        Layout.minimumWidth: 38
                                        Layout.maximumWidth: 38
                                        onClicked: backend.setDayStartHour(
                                                       backend.dayStartHour === backend.minDayStartHour
                                                       ? backend.maxDayStartHour : backend.dayStartHour - 1)
                                    }

                                    Label {
                                        objectName: "settingsDayStartHourValue"
                                        text: ("0" + backend.dayStartHour).slice(-2) + ":00"
                                        color: win.text
                                        font.pixelSize: 17
                                        font.family: "monospace"
                                        Layout.preferredWidth: 46
                                        Layout.minimumWidth: 46
                                        Layout.maximumWidth: 46
                                        horizontalAlignment: Text.AlignHCenter
                                    }

                                    ActionButton {
                                        objectName: "settingsDayStartHourPlus"
                                        compact: true
                                        label: "+"
                                        implicitWidth: 38
                                        Layout.minimumWidth: 38
                                        Layout.maximumWidth: 38
                                        onClicked: backend.setDayStartHour(
                                                       backend.dayStartHour === backend.maxDayStartHour
                                                       ? backend.minDayStartHour : backend.dayStartHour + 1)
                                    }
                                }
                            }

                            Panel {
                                objectName: "settingsRetentionPanel"
                                Layout.fillWidth: true
                                implicitHeight: retentionColumn.implicitHeight + 32

                                ColumnLayout {
                                    id: retentionColumn
                                    anchors.fill: parent
                                    anchors.margins: 16
                                    spacing: 6

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Label {
                                            objectName: "settingsRetentionTitle"
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            text: qsTr("Desired retention")
                                            color: win.text
                                            font.pixelSize: 14
                                            wrapMode: Text.Wrap
                                        }
                                        Label {
                                            id: retentionValue
                                            objectName: "settingsRetentionValue"
                                            Layout.minimumWidth: retentionValue.implicitWidth
                                            text: Math.round(retentionSlider.value * 100) + "%"
                                            color: theme.accent
                                            font.pixelSize: 14
                                            font.weight: Font.DemiBold
                                        }
                                    }

                                    Slider {
                                        id: retentionSlider
                                        objectName: "settingsRetentionSlider"
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        from: backend.minDesiredRetention
                                        to: backend.maxDesiredRetention
                                        stepSize: 0.01
                                        snapMode: Slider.SnapAlways
                                        value: backend.desiredRetention
                                        onMoved: backend.setDesiredRetention(value)
                                    }

                                    Label {
                                        objectName: "settingsRetentionDescription"
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        text: qsTr("Higher retention shortens intervals and means more reviews.")
                                        color: win.muted
                                        font.pixelSize: 11
                                        wrapMode: Text.Wrap
                                    }
                                }
                            }

                            Panel {
                                objectName: "settingsMathPanel"
                                Layout.fillWidth: true
                                implicitHeight: mathRow.implicitHeight + 32

                                RowLayout {
                                    id: mathRow
                                    anchors.fill: parent
                                    anchors.margins: 16
                                    spacing: 12

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        spacing: 2
                                        Label {
                                            objectName: "settingsMathTitle"
                                            Layout.fillWidth: true
                                            text: qsTr("Math")
                                            color: win.text
                                            font.pixelSize: 14
                                            wrapMode: Text.Wrap
                                        }
                                        Label {
                                            objectName: "settingsMathDescription"
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            text: qsTr("Cached under ~/.cache/omacards/math")
                                            color: win.muted
                                            font.pixelSize: 11
                                            wrapMode: Text.Wrap
                                        }
                                    }

                                    Label {
                                        id: mathEngine
                                        objectName: "settingsMathEngine"
                                        Layout.minimumWidth: mathEngine.implicitWidth
                                        text: backend.mathEngine
                                        color: win.muted
                                        font.pixelSize: 12
                                    }
                                }
                            }

                            Panel {
                                objectName: "settingsDeckFilesPanel"
                                Layout.fillWidth: true
                                implicitHeight: deckRow.implicitHeight + 32

                                RowLayout {
                                    id: deckRow
                                    anchors.fill: parent
                                    anchors.margins: 16
                                    spacing: 12

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.minimumWidth: 0
                                        spacing: 2
                                        Label {
                                            objectName: "settingsDeckFilesTitle"
                                            Layout.fillWidth: true
                                            text: qsTr("Deck files")
                                            color: win.text
                                            font.pixelSize: 14
                                            wrapMode: Text.Wrap
                                        }
                                        Label {
                                            objectName: "settingsDeckFilesPath"
                                            Layout.fillWidth: true
                                            Layout.minimumWidth: 0
                                            text: backend.decksDirectory
                                            color: win.muted
                                            font.pixelSize: 11
                                            elide: Text.ElideMiddle
                                        }
                                    }

                                    ActionButton {
                                        id: deckEditButton
                                        objectName: "settingsDeckFilesEditButton"
                                        compact: true
                                        label: qsTr("Edit file")
                                        hint: "E"
                                        Layout.minimumWidth: deckEditButton.implicitWidth
                                        Layout.maximumWidth: deckEditButton.implicitWidth
                                        onClicked: backend.editDeckFile()
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: 4
                                Layout.bottomMargin: 8
                                spacing: 10

                                ActionButton {
                                    objectName: "settingsBack"
                                    label: qsTr("Back")
                                    hint: "Esc"
                                    onClicked: win.goHome()
                                }

                                Item { Layout.fillWidth: true }
                            }
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Rectangle {
                        objectName: "studyFooterDot"
                        width: 8
                        height: 8
                        radius: 4
                        color: backend.totalCards > 0 ? theme.green : win.muted
                    }

                    Label {
                        objectName: "studyStatusLabel"
                        visible: win.screen !== "study" || !win.cramDistributionVisible
                        text: win.screen === "study"
                              ? backend.status
                              : backend.currentDeck.length > 0
                                  ? qsTr("%1 · %2 cards · plain text").arg(backend.currentDeck).arg(backend.totalCards)
                                  : qsTr("No decks yet")
                        color: win.muted
                        font.pixelSize: 11
                        elide: Text.ElideRight
                        Layout.maximumWidth: 460
                    }

                    Row {
                        id: boxDistributionDisplay
                        objectName: "boxDistributionDisplay"
                        visible: win.screen === "study" && win.cramDistributionVisible
                        spacing: 10

                        Label {
                            objectName: "boxDistributionCount1"
                            text: qsTr("Box 1: %1").arg(backend.boxDistribution[0] || 0)
                            color: backend.currentLeitnerBox === 1 ? theme.green : win.muted
                            font.pixelSize: 11
                        }
                        Label {
                            objectName: "boxDistributionCount2"
                            text: qsTr("Box 2: %1").arg(backend.boxDistribution[1] || 0)
                            color: backend.currentLeitnerBox === 2 ? theme.green : win.muted
                            font.pixelSize: 11
                        }
                        Label {
                            objectName: "boxDistributionCount3"
                            text: qsTr("Box 3: %1").arg(backend.boxDistribution[2] || 0)
                            color: backend.currentLeitnerBox === 3 ? theme.green : win.muted
                            font.pixelSize: 11
                        }
                        Label {
                            objectName: "boxDistributionCount4"
                            text: qsTr("Box 4: %1").arg(backend.boxDistribution[3] || 0)
                            color: backend.currentLeitnerBox === 4 ? theme.green : win.muted
                            font.pixelSize: 11
                        }
                        Label {
                            objectName: "boxDistributionCount5"
                            text: qsTr("Box 5: %1").arg(backend.boxDistribution[4] || 0)
                            color: backend.currentLeitnerBox === 5 ? theme.green : win.muted
                            font.pixelSize: 11
                        }
                        Label {
                            objectName: "cramNextInLabel"
                            text: qsTr("next in %1 cards").arg(backend.cramNextInCards)
                            color: win.muted
                            font.pixelSize: 11
                        }
                    }

                    Item { Layout.fillWidth: true }

                    LinkButton {
                        text: qsTr("? help")
                        onClicked: win.helpVisible = !win.helpVisible
                    }
                }
            }
        }
    }

    // ------------------------------------------------------------ overlays
    Rectangle {
        anchors.fill: parent
        visible: win.helpVisible
        color: win.withAlpha(theme.background, 0.88)

        MouseArea {
            anchors.fill: parent
            onClicked: win.helpVisible = false
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 80, 540)
            implicitHeight: helpColumn.implicitHeight + 56
            radius: win.clampedRadius(12)
            color: win.panel
            border.width: 1
            border.color: win.withAlpha(theme.accent, 0.5)

            ColumnLayout {
                id: helpColumn
                anchors.centerIn: parent
                width: parent.width - 56
                spacing: 12

                Label {
                    text: qsTr("Keyboard")
                    color: win.text
                    font.pixelSize: 19
                    font.weight: Font.DemiBold
                }

                Repeater {
                    model: [
                        { keys: "Space", action: qsTr("Start review, always flip the card, or continue") },
                        { keys: "↑ / ↓", action: qsTr("Scroll a long card, or flip at the end") },
                        { keys: "1 / A / ←", action: qsTr("Again") },
                        { keys: "2 / G / →", action: qsTr("Good") },
                        { keys: "R / C", action: qsTr("Review or cram mode") },
                        {
                            keys: "[ / ]",
                            action: qsTr("Open the Deck selector on Home or in the Study header, move the Candidate deck, commit with Enter or cancel with Escape"),
                            objectName: "helpDeckSelectorKeysRow"
                        },
                        {
                            keys: "",
                            action: qsTr("Deck selector on Home or in the Study header: click to open, [ / ] moves the Candidate deck, Enter commits, Escape cancels"),
                            objectName: "helpDeckSelectorRow"
                        },
                        { keys: "B", action: qsTr("Open the library") },
                        { keys: "J / K", action: qsTr("Scroll a long card, or move the library selection") },
                        { keys: "Enter", action: qsTr("Edit the selected card in the library") },
                        { keys: "Del", action: qsTr("Delete the selected card in the library") },
                        { keys: "/", action: qsTr("Focus library search") },
                        { keys: "S", action: qsTr("Stats for today and this session") },
                        { keys: ",", action: qsTr("Settings") },
                        { keys: "N / D", action: qsTr("New card or new deck") },
                        { keys: "I / E", action: qsTr("Import a file, or edit the deck file") },
                        { keys: "Ctrl+Z", action: qsTr("Undo a card or deck edit") },
                        { keys: "Esc", action: qsTr("Close, or go back home") },
                        { keys: "Q", action: qsTr("Quit") }
                    ]

                    RowLayout {
                        objectName: modelData.objectName || ""
                        Layout.fillWidth: true
                        spacing: 20

                        Label {
                            text: modelData.keys
                            color: theme.accent
                            font.pixelSize: 13
                            font.family: "monospace"
                            Layout.preferredWidth: 100
                        }

                        Label {
                            objectName: modelData.objectName ? modelData.objectName + "Text" : ""
                            text: modelData.action
                            color: win.soft
                            font.pixelSize: 13
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                        }
                    }
                }

                Label {
                    Layout.topMargin: 4
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    text: qsTr("Review answers are not undoable, so a lapse cannot be erased by accident. Card and deck edits are.")
                    color: win.muted
                    font.pixelSize: 12
                }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        visible: win.statsVisible
        color: win.withAlpha(theme.background, 0.88)

        MouseArea {
            anchors.fill: parent
            onClicked: win.statsVisible = false
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 80, 460)
            implicitHeight: statsColumn.implicitHeight + 56
            radius: win.clampedRadius(12)
            color: win.panel
            border.width: 1
            border.color: win.withAlpha(theme.accent, 0.5)

            ColumnLayout {
                id: statsColumn
                anchors.centerIn: parent
                width: parent.width - 56
                spacing: 10

                Label {
                    text: qsTr("Stats")
                    color: win.text
                    font.pixelSize: 19
                    font.weight: Font.DemiBold
                }

                Repeater {
                    model: [
                        [qsTr("Reviews today"), (statsData.studyDayRatings || 0)],
                        [qsTr("Accuracy today"), (statsData.accuracy || 0) + "%"],
                        [qsTr("New cards today"), qsTr("%1 of %2").arg(statsData.newCardsOnStudyDay || 0).arg(statsData.newCardAllowance || 0)],
                        [qsTr("Due now"), (statsData.due || 0)],
                        [qsTr("Streak"), qsTr("%1 days").arg(statsData.streak || 0)],
                        [qsTr("Total reviews"), (statsData.totalRatings || 0)],
                        [qsTr("Cards"), (statsData.cards || 0)],
                        [qsTr("Decks"), (statsData.decks || 0)],
                        [qsTr("This session"), qsTr("%1 reviews · %2 Good").arg(statsData.sessionReviewed || 0).arg(statsData.sessionGood || 0)]
                    ]

                    RowLayout {
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: modelData[0]
                            color: win.muted
                            font.pixelSize: 13
                        }
                        Label {
                            text: modelData[1]
                            color: theme.accent
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }
                    }
                }

                Label {
                    Layout.topMargin: 4
                    text: qsTr("Press S or Esc to close")
                    color: win.muted
                    font.pixelSize: 11
                }
            }
        }
    }

    // ------------------------------------------------------------ dialogs
    Dialog {
        id: newDeckDialog
        modal: true
        anchors.centerIn: parent
        width: Math.min(420, win.width - 40)
        padding: 24
        closePolicy: Popup.CloseOnEscape
        background: Rectangle {
            color: win.panel
            radius: win.clampedRadius(12)
            border.width: 1
            border.color: win.border
        }
        onOpened: deckNameField.forceActiveFocus()
        onClosed: keyHandler.forceActiveFocus()

        ColumnLayout {
            anchors.fill: parent
            spacing: 12

            Label {
                text: qsTr("New deck")
                color: win.text
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }

            InputField {
                id: deckNameField
                Layout.fillWidth: true
                placeholderText: qsTr("Deck name")
                onAccepted: {
                    if (text.trim().length > 0) {
                        backend.newDeck(text.trim())
                        text = ""
                    }
                    newDeckDialog.close()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ActionButton {
                    label: qsTr("Cancel")
                    onClicked: newDeckDialog.close()
                }
                ActionButton {
                    primary: true
                    label: qsTr("Create")
                    onClicked: {
                        if (deckNameField.text.trim().length > 0) {
                            backend.newDeck(deckNameField.text.trim())
                            deckNameField.text = ""
                        }
                        newDeckDialog.close()
                    }
                }
            }
        }
    }

    Dialog {
        id: newCardDialog
        objectName: "newCardDialog"
        modal: true
        anchors.centerIn: parent
        width: Math.min(580, win.width - 40)
        padding: 24
        closePolicy: Popup.CloseOnEscape
        background: Rectangle {
            color: win.panel
            radius: win.clampedRadius(12)
            border.width: 1
            border.color: win.border
        }
        onOpened: newQuestion.forceActiveFocus()
        onClosed: keyHandler.forceActiveFocus()

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Label {
                text: qsTr("New card")
                color: win.text
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }

            TextBox {
                id: newQuestion
                Layout.fillWidth: true
                Layout.preferredHeight: 84
                placeholderText: qsTr("Question (LaTeX with \\( ... \\))")
            }

            TextBox {
                id: newAnswer
                Layout.fillWidth: true
                Layout.preferredHeight: 84
                placeholderText: qsTr("Answer (cloze cards: write the sentence in the question)")
            }

            RowLayout {
                Layout.fillWidth: true
                ComboBox {
                    id: newType
                    model: [qsTr("Basic"), qsTr("Cloze")]
                }
                InputField {
                    id: newTags
                    Layout.fillWidth: true
                    placeholderText: qsTr("Tags, comma separated")
                }
            }

            Label {
                Layout.fillWidth: true
                visible: backend.currentDeck.length === 0
                text: qsTr("Create a deck first with D.")
                color: theme.yellow
                font.pixelSize: 11
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ActionButton {
                    label: qsTr("Cancel")
                    onClicked: newCardDialog.close()
                }
                ActionButton {
                    primary: true
                    label: qsTr("Add card")
                    onClicked: {
                        backend.addCardDetails(newQuestion.text, newAnswer.text,
                                               newType.currentIndex === 1 ? "cloze" : "basic", newTags.text)
                        newQuestion.text = ""
                        newAnswer.text = ""
                        newTags.text = ""
                        newType.currentIndex = 0
                        newCardDialog.close()
                    }
                }
            }
        }
    }

    Dialog {
        id: editCardDialog
        objectName: "editCardDialog"
        modal: true
        anchors.centerIn: parent
        width: Math.min(580, win.width - 40)
        padding: 24
        closePolicy: Popup.CloseOnEscape
        background: Rectangle {
            color: win.panel
            radius: win.clampedRadius(12)
            border.width: 1
            border.color: win.border
        }
        onClosed: keyHandler.forceActiveFocus()

        ColumnLayout {
            anchors.fill: parent
            spacing: 10

            Label {
                text: qsTr("Edit card")
                color: win.text
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }

            TextBox {
                id: editQuestion
                Layout.fillWidth: true
                Layout.preferredHeight: 84
            }

            TextBox {
                id: editAnswer
                Layout.fillWidth: true
                Layout.preferredHeight: 84
            }

            RowLayout {
                Layout.fillWidth: true
                ComboBox {
                    id: editType
                    model: [qsTr("Basic"), qsTr("Cloze")]
                }
                InputField {
                    id: editTags
                    Layout.fillWidth: true
                    placeholderText: qsTr("Tags, comma separated")
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ActionButton {
                    label: qsTr("Cancel")
                    onClicked: editCardDialog.close()
                }
                ActionButton {
                    primary: true
                    label: qsTr("Save")
                    onClicked: {
                        backend.updateCard(win.editCardId, editQuestion.text, editAnswer.text,
                                           editType.currentIndex === 1 ? "cloze" : "basic", editTags.text)
                        editCardDialog.close()
                    }
                }
            }
        }
    }

    Dialog {
        id: deleteDeckDialog
        modal: true
        anchors.centerIn: parent
        width: Math.min(440, win.width - 40)
        padding: 24
        closePolicy: Popup.CloseOnEscape
        background: Rectangle {
            color: win.panel
            radius: win.clampedRadius(12)
            border.width: 1
            border.color: win.withAlpha(theme.red, 0.5)
        }
        onClosed: keyHandler.forceActiveFocus()

        ColumnLayout {
            anchors.fill: parent
            spacing: 12

            Label {
                text: qsTr("Delete deck?")
                color: win.text
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Remove “%1” and all of its cards? Ctrl+Z can undo this.")
                      .arg(win.pendingDeck)
                color: win.soft
                font.pixelSize: 13
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ActionButton {
                    label: qsTr("Cancel")
                    onClicked: deleteDeckDialog.close()
                }
                ActionButton {
                    danger: true
                    label: qsTr("Delete")
                    onClicked: {
                        backend.deleteDeck(win.pendingDeck)
                        deleteDeckDialog.close()
                    }
                }
            }
        }
    }

    Dialog {
        id: importDialog
        modal: true
        anchors.centerIn: parent
        width: Math.min(540, win.width - 40)
        padding: 24
        closePolicy: Popup.CloseOnEscape
        background: Rectangle {
            color: win.panel
            radius: win.clampedRadius(12)
            border.width: 1
            border.color: win.border
        }
        onOpened: importPathField.forceActiveFocus()
        onClosed: keyHandler.forceActiveFocus()

        ColumnLayout {
            anchors.fill: parent
            spacing: 12

            Label {
                text: qsTr("Import cards")
                color: win.text
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: qsTr("Anki .apkg, CSV or TSV into %1.").arg(backend.currentDeck)
                color: win.muted
                font.pixelSize: 12
            }

            InputField {
                id: importPathField
                Layout.fillWidth: true
                placeholderText: qsTr("Path to .apkg, .csv or .tsv")
                onAccepted: {
                    if (text.trim().length > 0)
                        backend.importFrom(text.trim(), backend.currentDeck)
                    text = ""
                    importDialog.close()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Item { Layout.fillWidth: true }
                ActionButton {
                    label: qsTr("Cancel")
                    onClicked: importDialog.close()
                }
                ActionButton {
                    primary: true
                    label: qsTr("Import")
                    onClicked: {
                        if (importPathField.text.trim().length > 0)
                            backend.importFrom(importPathField.text.trim(), backend.currentDeck)
                        importPathField.text = ""
                        importDialog.close()
                    }
                }
            }
        }
    }

    Popup {
        id: noticePopup
        objectName: "noticePopup"
        parent: win.contentItem
        x: (win.width - implicitWidth) / 2
        y: win.height - implicitHeight - 24
        visible: noticeTimer.running
        padding: 12
        background: Rectangle {
            color: win.panel
            radius: win.clampedRadius(8)
            border.width: 1
            border.color: win.withAlpha(theme.red, 0.6)
        }
        Label {
            objectName: "noticeLabel"
            text: noticeMessage
            color: win.text
            font.pixelSize: 12
        }
    }

    Timer {
        id: noticeTimer
        interval: 2600
    }

    Connections {
        target: backend
        function onErrorOccurred(message) {
            noticeMessage = message
            noticeTimer.restart()
        }
    }

    Component.onCompleted: keyHandler.forceActiveFocus()
}
