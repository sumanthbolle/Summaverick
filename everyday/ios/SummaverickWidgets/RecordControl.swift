// The Control Center / Lock Screen record control (iOS 18+; the Action button can also run it). A toggle: on while
// a recording is going. Turning it on or off runs RecordControlIntent in the app, which starts or stops the one
// recording (RecordingController). The state is what the app last wrote to the App Group (RecordControlState); the
// app reloads the control whenever a recording starts or ends.
import AppIntents
import SwiftUI
import WidgetKit

struct RecordControl: ControlWidget {
    var body: some ControlWidgetConfiguration {
        StaticControlConfiguration(kind: RecordControlState.kind, provider: Provider()) { isRecording in
            ControlWidgetToggle("Record", isOn: isRecording, action: RecordControlIntent()) { on in
                Label(on ? "Recording" : "Record", systemImage: on ? "stop.circle.fill" : "record.circle")
            }
            .tint(.red)
        }
        .displayName("Record")
        .description("Start or stop a recording for notes.")
    }

    struct Provider: ControlValueProvider {
        var previewValue: Bool { false }
        func currentValue() async throws -> Bool { RecordControlState.isRecording }
    }
}
