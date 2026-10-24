import bb.cascades 1.4

Page {
    id: rootPage
    actionBarVisibility: ChromeVisibility.Hidden

    property string editTaskId: ""
    property int createPriority: 1
    property int editPriority: 1

    function syncSheets() {
        if (appBridge.createSheetOpen && !createTaskSheet.opened) {
            newTitleField.text = "";
            newDescField.text = "";
            newDueField.text = "";
            newProjectField.text = appBridge.activeProject.length > 0 ? appBridge.activeProject : "inbox";
            rootPage.createPriority = 1;
            createTaskSheet.open();
        } else if (!appBridge.createSheetOpen && createTaskSheet.opened) {
            createTaskSheet.close();
            taskListView.requestFocus();
        }

        if (appBridge.detailSheetOpen) {
            var d = appBridge.detailTask;
            if (!taskDetailSheet.opened) {
                rootPage.editTaskId = d.id || "";
                detailTitleField.text = d.content || "";
                detailDescField.text = d.description || "";
                detailDueField.text = d.dueDate || "";
                rootPage.editPriority = d.priority || 1;
                taskDetailSheet.open();
            } else if (d.id && d.id !== rootPage.editTaskId) {
                rootPage.editTaskId = d.id;
            }
        } else if (taskDetailSheet.opened) {
            taskDetailSheet.close();
            taskListView.requestFocus();
        }

        if (appBridge.settingsSheetOpen && !settingsSheet.opened) {
            settingsSheet.open();
        } else if (!appBridge.settingsSheetOpen && settingsSheet.opened) {
            settingsSheet.close();
            taskListView.requestFocus();
        }
    }

    onCreationCompleted: {
        appBridge.sheetsChanged.connect(rootPage.syncSheets);
        appBridge.selectionChanged.connect(function() {
            if (!searchField.focused && appBridge.selectedIndex >= 0) {
                taskListView.scrollToItem([appBridge.selectedIndex], 2);
            }
        });
        appBridge.filterChanged.connect(function() {
            if (!searchField.focused && appBridge.selectedIndex >= 0) {
                taskListView.scrollToItem([appBridge.selectedIndex], 2);
            }
        });
        rootPage.syncSheets();
        taskListView.requestFocus();
    }

    shortcuts: [
        Shortcut {
            key: "Space"
            onTriggered: appBridge.handleShortcut("Space")
        },
        Shortcut {
            key: "Enter"
            onTriggered: appBridge.handleShortcut("Enter")
        },
        Shortcut {
            key: "Backspace"
            onTriggered: appBridge.handleShortcut("Backspace")
        },
        Shortcut {
            key: "Delete"
            onTriggered: appBridge.handleShortcut("Delete")
        },
        Shortcut {
            key: "c"
            onTriggered: appBridge.handleShortcut("c")
        },
        Shortcut {
            key: "C"
            onTriggered: appBridge.handleShortcut("C")
        },
        Shortcut {
            key: "n"
            onTriggered: appBridge.handleShortcut("n")
        },
        Shortcut {
            key: "N"
            onTriggered: appBridge.handleShortcut("N")
        },
        Shortcut {
            key: "r"
            onTriggered: appBridge.handleShortcut("r")
        },
        Shortcut {
            key: "R"
            onTriggered: appBridge.handleShortcut("R")
        },
        Shortcut {
            key: "j"
            onTriggered: appBridge.handleShortcut("j")
        },
        Shortcut {
            key: "k"
            onTriggered: appBridge.handleShortcut("k")
        },
        Shortcut {
            key: "Down"
            onTriggered: appBridge.handleShortcut("j")
        },
        Shortcut {
            key: "Up"
            onTriggered: appBridge.handleShortcut("k")
        },
        Shortcut {
            key: "Escape"
            onTriggered: appBridge.handleShortcut("Escape")
        }
    ]

    attachedObjects: [
        Sheet {
            id: createTaskSheet
            Page {
                shortcuts: [
                    Shortcut {
                        key: "Escape"
                        onTriggered: appBridge.closeSheets()
                    }
                ]
                titleBar: TitleBar {
                    title: "New Task [C]"
                    dismissAction: ActionItem {
                        title: "Cancel"
                        onTriggered: appBridge.closeSheets()
                    }
                    acceptAction: ActionItem {
                        title: "Add"
                        onTriggered: {
                            appBridge.createTask(
                                newTitleField.text,
                                newDescField.text,
                                rootPage.createPriority,
                                newDueField.text,
                                newProjectField.text
                            );
                        }
                    }
                }
                Container {
                    background: Color.create("#111722")
                    leftPadding: 32
                    rightPadding: 32
                    topPadding: 28
                    bottomPadding: 28

                    Label {
                        text: "Task Title"
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#94a3b8")
                    }
                    TextField {
                        id: newTitleField
                        hintText: "What needs to be done?"
                        maximumLength: 500
                    }

                    Label {
                        text: "Description"
                        topMargin: 16
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#94a3b8")
                    }
                    TextArea {
                        id: newDescField
                        hintText: "Notes or context (optional)"
                        preferredHeight: 180
                    }

                    Label {
                        text: "Priority"
                        topMargin: 16
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#94a3b8")
                    }
                    Container {
                        topMargin: 8
                        horizontalAlignment: HorizontalAlignment.Fill
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }

                        Container {
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            preferredHeight: 72
                            background: rootPage.createPriority === 4 ? Color.create("#ef4444") : Color.create("#1e293b")
                            layout: DockLayout {}
                            Label {
                                horizontalAlignment: HorizontalAlignment.Center
                                verticalAlignment: VerticalAlignment.Center
                                text: "P1 Urgent"
                                textStyle.color: rootPage.createPriority === 4 ? Color.White : Color.create("#ef4444")
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                            }
                            onTouch: { if (event.isUp()) rootPage.createPriority = 4; }
                        }
                        Container {
                            leftMargin: 8
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            preferredHeight: 72
                            background: rootPage.createPriority === 3 ? Color.create("#f97316") : Color.create("#1e293b")
                            layout: DockLayout {}
                            Label {
                                horizontalAlignment: HorizontalAlignment.Center
                                verticalAlignment: VerticalAlignment.Center
                                text: "P2 High"
                                textStyle.color: rootPage.createPriority === 3 ? Color.White : Color.create("#f97316")
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                            }
                            onTouch: { if (event.isUp()) rootPage.createPriority = 3; }
                        }
                        Container {
                            leftMargin: 8
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            preferredHeight: 72
                            background: rootPage.createPriority === 2 ? Color.create("#3b82f6") : Color.create("#1e293b")
                            layout: DockLayout {}
                            Label {
                                horizontalAlignment: HorizontalAlignment.Center
                                verticalAlignment: VerticalAlignment.Center
                                text: "P3 Med"
                                textStyle.color: rootPage.createPriority === 2 ? Color.White : Color.create("#3b82f6")
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                            }
                            onTouch: { if (event.isUp()) rootPage.createPriority = 2; }
                        }
                        Container {
                            leftMargin: 8
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            preferredHeight: 72
                            background: rootPage.createPriority === 1 ? Color.create("#64748b") : Color.create("#1e293b")
                            layout: DockLayout {}
                            Label {
                                horizontalAlignment: HorizontalAlignment.Center
                                verticalAlignment: VerticalAlignment.Center
                                text: "P4 Normal"
                                textStyle.color: rootPage.createPriority === 1 ? Color.White : Color.create("#94a3b8")
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                            }
                            onTouch: { if (event.isUp()) rootPage.createPriority = 1; }
                        }
                    }

                    Label {
                        text: "Due Date (YYYY-MM-DD)"
                        topMargin: 16
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#94a3b8")
                    }
                    TextField {
                        id: newDueField
                        hintText: "e.g. 2026-10-25 (leave empty for none)"
                        maximumLength: 32
                    }

                    Label {
                        text: "Project"
                        topMargin: 16
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#94a3b8")
                    }
                    TextField {
                        id: newProjectField
                        text: "inbox"
                        hintText: "inbox"
                        maximumLength: 120
                    }
                }
            }
        },
        Sheet {
            id: taskDetailSheet
            Page {
                shortcuts: [
                    Shortcut {
                        key: "Escape"
                        onTriggered: appBridge.closeSheets()
                    }
                ]
                titleBar: TitleBar {
                    title: "Task Details [Enter]"
                    dismissAction: ActionItem {
                        title: "Close"
                        onTriggered: appBridge.closeSheets()
                    }
                    acceptAction: ActionItem {
                        title: "Save"
                        onTriggered: {
                            appBridge.updateTask(
                                rootPage.editTaskId,
                                detailTitleField.text,
                                detailDescField.text,
                                rootPage.editPriority,
                                detailDueField.text
                            );
                        }
                    }
                }
                Container {
                    background: Color.create("#111722")
                    leftPadding: 32
                    rightPadding: 32
                    topPadding: 28
                    bottomPadding: 28

                    Label {
                        text: "Task Title"
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#94a3b8")
                    }
                    TextField {
                        id: detailTitleField
                        hintText: "Task title"
                        maximumLength: 500
                    }

                    Label {
                        text: "Description"
                        topMargin: 16
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#94a3b8")
                    }
                    TextArea {
                        id: detailDescField
                        hintText: "Description"
                        preferredHeight: 200
                    }

                    Label {
                        text: "Priority"
                        topMargin: 16
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#94a3b8")
                    }
                    Container {
                        topMargin: 8
                        horizontalAlignment: HorizontalAlignment.Fill
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }

                        Container {
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            preferredHeight: 72
                            background: rootPage.editPriority === 4 ? Color.create("#ef4444") : Color.create("#1e293b")
                            layout: DockLayout {}
                            Label {
                                horizontalAlignment: HorizontalAlignment.Center
                                verticalAlignment: VerticalAlignment.Center
                                text: "P1 Urgent"
                                textStyle.color: rootPage.editPriority === 4 ? Color.White : Color.create("#ef4444")
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                            }
                            onTouch: { if (event.isUp()) rootPage.editPriority = 4; }
                        }
                        Container {
                            leftMargin: 8
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            preferredHeight: 72
                            background: rootPage.editPriority === 3 ? Color.create("#f97316") : Color.create("#1e293b")
                            layout: DockLayout {}
                            Label {
                                horizontalAlignment: HorizontalAlignment.Center
                                verticalAlignment: VerticalAlignment.Center
                                text: "P2 High"
                                textStyle.color: rootPage.editPriority === 3 ? Color.White : Color.create("#f97316")
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                            }
                            onTouch: { if (event.isUp()) rootPage.editPriority = 3; }
                        }
                        Container {
                            leftMargin: 8
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            preferredHeight: 72
                            background: rootPage.editPriority === 2 ? Color.create("#3b82f6") : Color.create("#1e293b")
                            layout: DockLayout {}
                            Label {
                                horizontalAlignment: HorizontalAlignment.Center
                                verticalAlignment: VerticalAlignment.Center
                                text: "P3 Med"
                                textStyle.color: rootPage.editPriority === 2 ? Color.White : Color.create("#3b82f6")
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                            }
                            onTouch: { if (event.isUp()) rootPage.editPriority = 2; }
                        }
                        Container {
                            leftMargin: 8
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            preferredHeight: 72
                            background: rootPage.editPriority === 1 ? Color.create("#64748b") : Color.create("#1e293b")
                            layout: DockLayout {}
                            Label {
                                horizontalAlignment: HorizontalAlignment.Center
                                verticalAlignment: VerticalAlignment.Center
                                text: "P4 Normal"
                                textStyle.color: rootPage.editPriority === 1 ? Color.White : Color.create("#94a3b8")
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                            }
                            onTouch: { if (event.isUp()) rootPage.editPriority = 1; }
                        }
                    }

                    Label {
                        text: "Due Date (YYYY-MM-DD, clear to remove)"
                        topMargin: 16
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#94a3b8")
                    }
                    TextField {
                        id: detailDueField
                        hintText: "YYYY-MM-DD"
                        maximumLength: 32
                    }

                    Container {
                        topMargin: 28
                        horizontalAlignment: HorizontalAlignment.Fill
                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }

                        Button {
                            text: appBridge.detailTask.isCompleted ? "Reopen Task" : "Complete Task"
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            onClicked: {
                                appBridge.toggleTaskCompleted(rootPage.editTaskId);
                                appBridge.closeSheets();
                            }
                        }
                        Button {
                            text: "Delete Task"
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            onClicked: {
                                appBridge.deleteTask(rootPage.editTaskId);
                            }
                        }
                    }
                }
            }
        },
        Sheet {
            id: settingsSheet
            Page {
                shortcuts: [
                    Shortcut {
                        key: "Escape"
                        onTriggered: appBridge.closeSheets()
                    }
                ]
                titleBar: TitleBar {
                    title: "Todoist Sync & Token"
                    dismissAction: ActionItem {
                        title: "Close"
                        onTriggered: {
                            inboxProjectInputField.text = appBridge.inboxProjectId;
                            appBridge.closeSheets();
                        }
                    }
                    acceptAction: ActionItem {
                        title: "Save"
                        onTriggered: {
                            var trimmedInbox = inboxProjectInputField.text.trim();
                            if (trimmedInbox.length > 0 && trimmedInbox !== appBridge.inboxProjectId) {
                                if (!appBridge.setInboxProjectId(trimmedInbox)) {
                                    return;
                                }
                            }
                            if (tokenInputField.text.length > 0) {
                                if (!appBridge.saveApiToken(tokenInputField.text)) {
                                    return;
                                }
                                tokenInputField.text = "";
                            }
                            appBridge.closeSheets();
                        }
                    }
                }
                Container {
                    background: Color.create("#111722")
                    leftPadding: 32
                    rightPadding: 32
                    topPadding: 28
                    bottomPadding: 28

                    Label {
                        text: "Todoist Sync API v1"
                        textStyle.base: SystemDefaults.TextStyles.TitleText
                        textStyle.color: Color.White
                    }
                    Label {
                        text: "Endpoint: https://api.todoist.com/api/v1/sync (TLS 1.3 / Bundled Mozilla CA)"
                        multiline: true
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#94a3b8")
                    }
                    Label {
                        text: appBridge.hasToken ? ("Saved Token: " + appBridge.maskedToken) : "No API token configured"
                        topMargin: 16
                        textStyle.color: appBridge.hasToken ? Color.create("#4ade80") : Color.create("#f87171")
                    }
                    TextField {
                        id: tokenInputField
                        topMargin: 12
                        hintText: "Paste Personal API Token"
                        maximumLength: 256
                    }
                    Label {
                        text: "Server Inbox Project ID"
                        topMargin: 16
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#94a3b8")
                    }
                    TextField {
                        id: inboxProjectInputField
                        topMargin: 8
                        hintText: "Enter Server Inbox Project ID"
                        text: appBridge.inboxProjectId
                        maximumLength: 128
                    }
                    Label {
                        text: appBridge.errorText
                        visible: appBridge.errorText.length > 0
                        topMargin: 12
                        multiline: true
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#f87171")
                    }
                    Label {
                        text: "Pending outbound commands: " + appBridge.pendingSyncCount + "  ·  Dead-letter queue: " + appBridge.deadLetterCount
                        topMargin: 16
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#cbd5e1")
                    }
                    Button {
                        text: "Re-sync All (Reconcile Projects)"
                        topMargin: 24
                        enabled: appBridge.hasToken && !appBridge.busy
                        onClicked: {
                            if (appBridge.triggerFullSync()) {
                                appBridge.closeSheets();
                            }
                        }
                    }
                    Button {
                        text: "Clear Saved API Token"
                        topMargin: 12
                        enabled: appBridge.hasToken && !appBridge.busy
                        onClicked: appBridge.clearApiToken()
                    }
                }
            }
        },
        Sheet {
            id: projectPickerSheet
            onClosed: {
                taskListView.requestFocus();
            }
            Page {
                shortcuts: [
                    Shortcut {
                        key: "Escape"
                        onTriggered: projectPickerSheet.close()
                    }
                ]
                titleBar: TitleBar {
                    title: "Select Project"
                    dismissAction: ActionItem {
                        title: "Close"
                        onTriggered: projectPickerSheet.close()
                    }
                }
                Container {
                    background: Color.create("#111722")
                    horizontalAlignment: HorizontalAlignment.Fill
                    verticalAlignment: VerticalAlignment.Fill
                    leftPadding: 24
                    rightPadding: 24
                    topPadding: 20
                    bottomPadding: 20

                    Label {
                        text: "Available Projects"
                        textStyle.base: SystemDefaults.TextStyles.TitleText
                        textStyle.color: Color.White
                    }
                    ListView {
                        dataModel: appBridge.projectModel
                        topMargin: 16
                        horizontalAlignment: HorizontalAlignment.Fill
                        listItemComponents: [
                            ListItemComponent {
                                type: ""
                                Container {
                                    horizontalAlignment: HorizontalAlignment.Fill
                                    leftPadding: 20
                                    rightPadding: 20
                                    topPadding: 16
                                    bottomPadding: 16
                                    bottomMargin: 8
                                    background: Color.create("#151c28")
                                    Label {
                                        text: "# " + ListItemData
                                        textStyle.base: SystemDefaults.TextStyles.BodyText
                                        textStyle.color: Color.White
                                    }
                                }
                            }
                        ]
                        onTriggered: {
                            if (indexPath.length > 0) {
                                var proj = dataModel.data(indexPath);
                                appBridge.selectProject(proj);
                                projectPickerSheet.close();
                            }
                        }
                    }
                }
            }
        }
    ]

    // Root 1440x1440 Square Viewport Container
    Container {
        background: Color.create("#0f141d")
        horizontalAlignment: HorizontalAlignment.Fill
        verticalAlignment: VerticalAlignment.Fill
        leftPadding: 24
        rightPadding: 24
        topPadding: 20
        bottomPadding: 16

        // 1. Top Header Bar (Brand + Sync Status + Quick Actions)
        Container {
            horizontalAlignment: HorizontalAlignment.Fill
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }

            Container {
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                verticalAlignment: VerticalAlignment.Center
                Label {
                    text: "Todoist"
                    textStyle.base: SystemDefaults.TextStyles.TitleText
                    textStyle.color: Color.create("#E44332")
                }
                Label {
                    text: appBridge.statusText + (appBridge.pendingSyncCount > 0 ? (" · " + appBridge.pendingSyncCount + " pending") : "")
                    textFormat: TextFormat.Plain
                    textStyle.base: SystemDefaults.TextStyles.SmallText
                    textStyle.color: appBridge.requiresAuth ? Color.create("#f87171") : Color.create("#94a3b8")
                }
            }

            ActivityIndicator {
                preferredWidth: 44
                preferredHeight: 44
                verticalAlignment: VerticalAlignment.Center
                running: appBridge.busy
                visible: appBridge.busy
            }

            Button {
                text: "+ Task [C]"
                preferredWidth: 210
                verticalAlignment: VerticalAlignment.Center
                onClicked: appBridge.openCreateSheet()
            }
            Button {
                text: "Sync [R]"
                preferredWidth: 190
                verticalAlignment: VerticalAlignment.Center
                enabled: !appBridge.busy
                onClicked: appBridge.triggerSync()
            }
            Button {
                text: "Token"
                preferredWidth: 170
                verticalAlignment: VerticalAlignment.Center
                onClicked: appBridge.openSettingsSheet()
            }
        }

        // 2. Error / Auth Notice Banner
        Container {
            visible: appBridge.errorText.length > 0 || appBridge.requiresAuth
            background: Color.create("#3b181e")
            horizontalAlignment: HorizontalAlignment.Fill
            topMargin: 10
            leftPadding: 18
            rightPadding: 18
            topPadding: 10
            bottomPadding: 10
            Label {
                text: appBridge.errorText.length > 0 ? appBridge.errorText : "Personal API Token required for cloud sync."
                multiline: true
                textFormat: TextFormat.Plain
                textStyle.base: SystemDefaults.TextStyles.SmallText
                textStyle.color: Color.create("#fca5a5")
            }
        }

        // 3. Filter Navigation Strip (1440px horizontal density)
        Container {
            topMargin: 12
            horizontalAlignment: HorizontalAlignment.Fill
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }

            Button {
                text: "Inbox"
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                onClicked: {
                    appBridge.setFilter("inbox", "inbox");
                    taskListView.requestFocus();
                }
            }
            Button {
                text: "Today (" + appBridge.todayIncompleteCount + ")"
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                onClicked: {
                    appBridge.setFilter("today", "");
                    taskListView.requestFocus();
                }
            }
            Button {
                text: "All (" + appBridge.totalIncompleteCount + ")"
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                onClicked: {
                    appBridge.setFilter("all", "");
                    taskListView.requestFocus();
                }
            }
            Button {
                text: "Done (" + appBridge.completedCount + ")"
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                onClicked: {
                    appBridge.setFilter("completed", "");
                    taskListView.requestFocus();
                }
            }
            Button {
                text: appBridge.filterMode === "project" ? ("#" + appBridge.projectDisplayName(appBridge.activeProject)) : "Projects"
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                onClicked: {
                    appBridge.refreshProjects();
                    projectPickerSheet.open();
                }
            }
        }

        // 4. Search & Shortcut Legend Row
        Container {
            topMargin: 10
            horizontalAlignment: HorizontalAlignment.Fill
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }

            TextField {
                id: searchField
                layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                hintText: "Filter tasks by title, note, or project..."
                text: appBridge.searchQuery
                onTextChanging: {
                    if (appBridge.searchQuery !== text) {
                        appBridge.setSearchQuery(text);
                    }
                }
                input.submitKey: SubmitKey.Search
                input.onSubmitted: {
                    taskListView.requestFocus();
                }
            }
        }

        // 5. Primary Task ListView (bound to CascadesTaskListModel : bb::cascades::DataModel)
        ListView {
            id: taskListView
            topMargin: 12
            horizontalAlignment: HorizontalAlignment.Fill
            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
            dataModel: appBridge.taskModel
            scrollRole: ScrollRole.Main
            flickMode: FlickMode.Momentum
            // Let the page's Space shortcut complete tasks instead of paging.
            onCreationCompleted: setBuiltInShortcutEnabled(SystemShortcuts.ScrollDownOneScreen, false)

            eventHandlers: [
                TrackpadHandler {
                    onTrackpad: {
                        if (event.isMove()) {
                            taskListView.scroll(-event.deltaY * 3, 1);
                        }
                    }
                },
                NavigationHandler {
                    onNavigation: {
                        if (event.navigationEventType === NavigationEventType.Down) {
                            appBridge.handleShortcut("j");
                        } else if (event.navigationEventType === NavigationEventType.Up) {
                            appBridge.handleShortcut("k");
                        }
                    }
                }
            ]

            property real touchStartY: 0
            property bool pullTriggered: false
            property bool pullEligible: false
            property bool atBeginning: scrollHandler.atBeginning

            attachedObjects: [
                ListScrollStateHandler {
                    id: scrollHandler
                }
            ]

            onTouch: {
                if (event.isDown()) {
                    touchStartY = event.windowY;
                    pullTriggered = false;
                    pullEligible = (typeof atBeginning !== 'undefined') ? atBeginning : true;
                } else if (event.isMove()) {
                    if (pullEligible && (event.windowY - touchStartY > 100)) {
                        pullTriggered = true;
                    }
                } else if (event.isCancel()) {
                    pullTriggered = false;
                    pullEligible = false;
                } else if (event.isUp()) {
                    pullEligible = false;
                    if (pullTriggered) {
                        pullTriggered = false;
                        if (!appBridge.busy) {
                            appBridge.triggerSync();
                        }
                    }
                }
            }

            leadingVisualSnapThreshold: 0.2
            leadingVisual: Container {
                horizontalAlignment: HorizontalAlignment.Fill
                preferredHeight: 110
                background: Color.create("#111722")
                layout: DockLayout {}
                Label {
                    horizontalAlignment: HorizontalAlignment.Center
                    verticalAlignment: VerticalAlignment.Center
                    text: appBridge.busy ? "Syncing..." : (taskListView.pullTriggered ? "Release to refresh" : "Pull down to refresh")
                    textStyle.base: SystemDefaults.TextStyles.BodyText
                    textStyle.color: Color.create("#8fa0b5")
                }
            }

            listItemComponents: [
                ListItemComponent {
                    type: "task"
                    Container {
                        id: itemRoot
                        horizontalAlignment: HorizontalAlignment.Fill
                        background: ListItemData.isSelected ? Color.create("#1e293b") : Color.create("#151c28")
                        leftPadding: 16
                        rightPadding: 16
                        topPadding: 14
                        bottomPadding: 14
                        bottomMargin: 8

                        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }

                        // Interactive Checkbox / Touch Completion Target (M4-DEV-002)
                        Container {
                            preferredWidth: 64
                            preferredHeight: 64
                            verticalAlignment: VerticalAlignment.Center
                            background: ListItemData.isCompleted ? Color.create("#15803d") : Color.create("#1e293b")
                            layout: DockLayout {}

                            Label {
                                horizontalAlignment: HorizontalAlignment.Center
                                verticalAlignment: VerticalAlignment.Center
                                text: ListItemData.isCompleted ? "✓" : ""
                                textStyle.base: SystemDefaults.TextStyles.TitleText
                                textStyle.color: Color.White
                            }

                            onTouch: {
                                if (event.isUp()) {
                                    appBridge.toggleTaskCompleted(ListItemData.id);
                                }
                            }
                        }

                        // Priority Badge
                        Container {
                            leftMargin: 12
                            preferredWidth: 60
                            verticalAlignment: VerticalAlignment.Center
                            Label {
                                text: ListItemData.priorityLabel
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                                textStyle.color: Color.create(ListItemData.priorityColor)
                            }
                        }

                        // Content + Metadata Column
                        Container {
                            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
                            leftPadding: 8
                            rightPadding: 8

                            Label {
                                text: ListItemData.content
                                textFormat: TextFormat.Plain
                                multiline: true
                                textStyle.base: SystemDefaults.TextStyles.BodyText
                                textStyle.color: ListItemData.isCompleted ? Color.create("#64748b") : Color.White
                            }

                            Label {
                                visible: ListItemData.description.length > 0
                                text: ListItemData.description
                                textFormat: TextFormat.Plain
                                multiline: true
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                                textStyle.color: Color.create("#94a3b8")
                            }

                            Label {
                                text: "#" + ListItemData.projectDisplayName +
                                      (ListItemData.dueDate.length > 0 ? ("  ·  Due " + ListItemData.dueDate) : "") +
                                      "  ·  " + ListItemData.syncBadge
                                textFormat: TextFormat.Plain
                                textStyle.base: SystemDefaults.TextStyles.SmallText
                                textStyle.color: Color.create("#64748b")
                            }
                        }
                    }
                }
            ]

            onTriggered: {
                if (indexPath.length > 0) {
                    var row = indexPath[0];
                    if (row === appBridge.selectedIndex) {
                        appBridge.openSelectedTaskDetail();
                    } else {
                        appBridge.selectTask(row);
                    }
                }
            }
        }

        // 6. Bottom Physical Keyboard Shortcut Footer Bar
        Container {
            topMargin: 8
            horizontalAlignment: HorizontalAlignment.Fill
            layout: StackLayout { orientation: LayoutOrientation.LeftToRight }

            Label {
                text: "Keys: [Space] Complete  ·  [Enter] Edit  ·  [Del] Delete  ·  [C] New  ·  [R] Sync  ·  [J/K] Move"
                horizontalAlignment: HorizontalAlignment.Fill
                textStyle.textAlign: TextAlign.Center
                textStyle.base: SystemDefaults.TextStyles.SmallText
                textStyle.color: Color.create("#64748b")
            }
        }
    }
}
