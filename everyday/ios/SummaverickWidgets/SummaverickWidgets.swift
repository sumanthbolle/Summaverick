// The widget extension: the recording's Live Activity and the Control Center / Lock Screen record control.
import SwiftUI
import WidgetKit

@main
struct SummaverickWidgets: WidgetBundle {
    var body: some Widget {
        RecordingLiveActivity()
        RecordControl()
    }
}
