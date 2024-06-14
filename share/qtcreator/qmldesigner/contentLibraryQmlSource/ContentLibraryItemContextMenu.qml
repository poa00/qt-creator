// Copyright (C) 2024 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0

import QtQuick
import StudioControls as StudioControls
import StudioTheme as StudioTheme
import ContentLibraryBackend

StudioControls.Menu {
    id: root

    property var targetItem: null
    property bool enableRemove: false // true: adds an option to remove targetItem

    readonly property bool targetAvailable: targetItem && !ContentLibraryBackend.rootView.importerRunning

    signal unimport();
    signal addToProject()
    signal applyToSelected(bool add)
    signal removeFromContentLib()

    function popupMenu(item = null)
    {
        root.targetItem = item

        let isMaterial = root.targetItem.bundleId === "UserMaterials"
        applyToSelectedReplace.visible = isMaterial
        applyToSelectedAdd.visible = isMaterial

        popup()
    }

    closePolicy: StudioControls.Menu.CloseOnEscape | StudioControls.Menu.CloseOnPressOutside

    StudioControls.MenuItem {
        id: applyToSelectedReplace
        text: qsTr("Apply to selected (replace)")
        height: visible ? implicitHeight : 0
        enabled: root.targetAvailable && ContentLibraryBackend.rootView.hasModelSelection
        onTriggered: root.applyToSelected(false)
    }

    StudioControls.MenuItem {
        id: applyToSelectedAdd
        text: qsTr("Apply to selected (add)")
        height: visible ? implicitHeight : 0
        enabled: root.targetAvailable && ContentLibraryBackend.rootView.hasModelSelection
        onTriggered: root.applyToSelected(true)
    }

    StudioControls.MenuSeparator {}

    StudioControls.MenuItem {
        enabled: root.targetAvailable
        text: qsTr("Add an instance to project")
        onTriggered: root.addToProject()
    }

    StudioControls.MenuItem {
        enabled: root.targetAvailable && root.targetItem.bundleItemImported
        text: qsTr("Remove from project")
        onTriggered: root.unimport()
    }

    StudioControls.MenuItem {
        text: qsTr("Remove from Content Library")
        visible: root.enableRemove && root.targetAvailable
        height: visible ? implicitHeight : 0
        onTriggered: root.removeFromContentLib()
    }
}
