import bb.cascades 1.4

Container {
    id: activeFrameRoot
    background: Color.create("#0f141d")
    horizontalAlignment: HorizontalAlignment.Fill
    verticalAlignment: VerticalAlignment.Fill
    leftPadding: 18
    rightPadding: 18
    topPadding: 16
    bottomPadding: 16

    // Header Row
    Container {
        horizontalAlignment: HorizontalAlignment.Fill
        layout: StackLayout { orientation: LayoutOrientation.LeftToRight }

        Label {
            text: "Todoist"
            layoutProperties: StackLayoutProperties { spaceQuota: 1 }
            textStyle.base: SystemDefaults.TextStyles.BodyText
            textStyle.color: Color.create("#E44332")
        }
        Label {
            text: appBridge.coverHeadline
            textFormat: TextFormat.Plain
            textStyle.base: SystemDefaults.TextStyles.SmallText
            textStyle.color: Color.create("#94a3b8")
        }
    }

    // Counter Summary
    Label {
        topMargin: 6
        text: appBridge.totalIncompleteCount + " active  ·  " + appBridge.todayIncompleteCount + " due today"
        textFormat: TextFormat.Plain
        textStyle.base: SystemDefaults.TextStyles.SmallText
        textStyle.color: Color.White
    }

    // Top Incomplete Tasks Preview List
    ListView {
        topMargin: 10
        horizontalAlignment: HorizontalAlignment.Fill
        layoutProperties: StackLayoutProperties { spaceQuota: 1 }
        dataModel: appBridge.coverTaskModel

        listItemComponents: [
            ListItemComponent {
                type: ""
                Container {
                    bottomMargin: 6
                    Label {
                        text: ListItemData.priorityLabel + "  " + ListItemData.content
                        textFormat: TextFormat.Plain
                        textStyle.base: SystemDefaults.TextStyles.SmallText
                        textStyle.color: Color.create("#e2e8f0")
                    }
                }
            }
        ]
    }
}
