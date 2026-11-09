// The one heat ladder state (HeatLadder in ModelLogic) for the whole app: live transcription and the pipeline read
// the same level, so they agree. It samples heat, battery and Low Power Mode whenever asked, on a timer while above
// full (so a level comes down by itself after its minute of calm), and when heat or Low Power Mode changes. Each
// level change is logged, posted (.heatLevelChanged) and handed to CoreEngine as CPU threads.
// Free memory is logged only (R-N6-b) until it is measured on the phone with the model loaded: a reading under
// 100 MB there would pin the phone at minimal, and neither knob lowers memory.
import Foundation
import os
import UIKit

extension Notification.Name {
    /// The heat ladder's level changed (main thread).
    static let heatLevelChanged = Notification.Name("sve.heatLevelChanged")
}

/// CPU threads the heat ladder wants, readable on any thread: CoreEngine applies them before each CPU run.
final class HeatThreads: @unchecked Sendable {
    private let lock = NSLock()
    private var value: (decode: Int32, batch: Int32) = (Int32(HeatLadder.cpuThreads(.full)), Int32(HeatLadder.batchThreads(.full)))
    var wanted: (decode: Int32, batch: Int32) { lock.lock(); defer { lock.unlock() }; return value }
    func set(_ level: HeatLadder.Level) {
        lock.lock()
        value = (Int32(HeatLadder.cpuThreads(level)), Int32(HeatLadder.batchThreads(level)))
        lock.unlock()
    }
}

@MainActor
final class HeatGovernor {
    static let shared = HeatGovernor()
    nonisolated static let threads = HeatThreads()

    private var state = HeatLadder.State()
    private var timer: Timer?
    private var releaseTimer: Timer?
    /// The governor switched battery monitoring on (it keeps it on while above full or while work reads the level).
    private(set) var holdsBatteryMonitoring = false

    /// Seconds on ContinuousClock, which keeps counting while the device sleeps (systemUptime, mach_absolute_time,
    /// stops then), so a suspended or asleep phone shows up as a gap and the minute of calm is real time.
    nonisolated static var now: TimeInterval {
        let c = origin.duration(to: .now).components
        return Double(c.seconds) + Double(c.attoseconds) * 1e-18
    }
    nonisolated private static let origin = ContinuousClock.now

    /// Free memory before the app's limit in MB, Int.max when no limit applies (simulator, Mac).
    nonisolated static var availableMemoryMB: Int {
        let available = os_proc_available_memory()
        return available == 0 ? Int.max : Int(available / 1_048_576)
    }
    private var observers: [NSObjectProtocol] = []

    private init() {
        for name in [ProcessInfo.thermalStateDidChangeNotification, Notification.Name.NSProcessInfoPowerStateDidChange] {
            observers.append(NotificationCenter.default.addObserver(forName: name, object: nil, queue: .main) { _ in
                MainActor.assumeIsolated { _ = HeatGovernor.shared.level }
            })
        }
    }

    /// The level now (samples the inputs first).
    var level: HeatLadder.Level {
        let thermal = ProcessInfo.processInfo.thermalState
        let memoryMB = Self.availableMemoryMB   // logged only (R-N6-b)
        let (battery, charging) = readBattery()
        let lowPower = ProcessInfo.processInfo.isLowPowerModeEnabled
        let demanded = HeatLadder.demanded(thermal: thermal, availableMemoryMB: Int.max, batteryLevel: battery,
                                           charging: charging, lowPower: lowPower)
        let before = state.level
        if state.update(demanded: demanded, now: Self.now) {
            NSLog("[sve] heat: %@ -> %@ (thermal %ld, mem %d MB, battery %.2f)", before.name, state.level.name,
                  thermal.rawValue, Int32(clamping: memoryMB), battery)
            Self.threads.set(state.level)
            NotificationCenter.default.post(name: .heatLevelChanged, object: nil)
        }
        keepTicking()
        return state.level
    }

    /// While above full, a reading every few seconds lets the level come down by itself.
    /// Also keeps battery monitoring held: while above full, and at full until a minute passes with no reading
    /// (work reads the level before every piece, section and output).
    private func keepTicking() {
        releaseTimer?.invalidate()
        releaseTimer = nil
        if state.level == .full {
            timer?.invalidate()
            timer = nil
            if holdsBatteryMonitoring {
                releaseTimer = Timer.scheduledTimer(withTimeInterval: HeatLadder.calmSeconds, repeats: false) { _ in
                    MainActor.assumeIsolated { HeatGovernor.shared.releaseBatteryMonitoring() }
                }
            }
        } else if timer == nil {
            timer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
                MainActor.assumeIsolated { _ = HeatGovernor.shared.level }
            }
        }
    }

    /// No reading for a minute at full: monitoring goes back off, unless a recording uses it (live transcription
    /// switches it off itself when it ends, unless the governor holds it).
    private func releaseBatteryMonitoring() {
        releaseTimer = nil
        guard holdsBatteryMonitoring, state.level == .full else { return }
        holdsBatteryMonitoring = false
        if !LiveTranscriber.shared.isRecording { UIDevice.current.isBatteryMonitoringEnabled = false }
    }

    /// Battery level (negative when unknown, e.g. just after monitoring is switched on) and charging.
    private func readBattery() -> (Float, Bool) {
        let d = UIDevice.current
        if !d.isBatteryMonitoringEnabled {
            d.isBatteryMonitoringEnabled = true
            holdsBatteryMonitoring = true
        }
        return (d.batteryLevel, d.batteryState == .charging || d.batteryState == .full)
    }
}
