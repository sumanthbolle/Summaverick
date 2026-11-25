// The one owner of UIApplication.isIdleTimerDisabled (ScreenAwakeRule): the screen stays on while the Finish now
// mode is on, or while the pipeline runs notes in front (a locked phone stops GPU work). Each side says what it wants
// and calls update(); nobody sets the idle timer directly, so the pipeline ending a run never lets the screen sleep
// under the mode, and leaving the mode never lets it sleep under a run in front.
import UIKit

@MainActor
enum ScreenAwake {
    /// Set by the pipeline: a run started (beginBackground) or ended (endBackground), or memory work that may lock.
    private(set) static var pipelineWants = false

    static func pipeline(wants: Bool) {
        pipelineWants = wants
        update()
    }

    static func update() {
        let disabled = ScreenAwakeRule.idleTimerDisabled(modeOn: FinishNowController.shared.session.isOn,
                                                         pipelineWants: pipelineWants)
        if UIApplication.shared.isIdleTimerDisabled != disabled { UIApplication.shared.isIdleTimerDisabled = disabled }
    }
}
