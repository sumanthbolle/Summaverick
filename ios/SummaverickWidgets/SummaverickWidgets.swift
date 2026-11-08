// The widget extension: the recording's Live Activity (R3 adds the Control Center record control).
import SwiftUI
import WidgetKit

@main
struct SummaverickWidgets: WidgetBundle {
    var body: some Widget {
        RecordingLiveActivity()
    }
}
