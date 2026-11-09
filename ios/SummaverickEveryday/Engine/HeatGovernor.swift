// The one heat ladder state (HeatLadder in ModelLogic) for the whole app: live transcription and the pipeline read
// the same level, so they agree. It samples heat, free memory, battery and Low Power Mode whenever asked, on a
// timer while above full (so a level comes down by itself after its minute of calm), and when heat or Low Power
// Mode changes. Each level change is logged, posted (.heatLevelChanged) and handed to CoreEngine as CPU threads.
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
        // 0 means no memory limit applies (simulator, Mac): unknown, never counts.
        let available = os_proc_available_memory()
        let memoryMB = available == 0 ? Int.max : Int(available / 1_048_576)
        let (battery, charging) = Self.battery()
        let lowPower = ProcessInfo.processInfo.isLowPowerModeEnabled
        let demanded = HeatLadder.demanded(thermal: thermal, availableMemoryMB: memoryMB, batteryLevel: battery,
                                           charging: charging, lowPower: lowPower)
        let before = state.level
        if state.update(demanded: demanded, now: Date()) {
            NSLog("[sve] heat: %@ -> %@ (thermal %ld, mem %d MB, battery %.2f)", before.name, state.level.name,
                  thermal.rawValue, Int32(clamping: memoryMB), battery)
            Self.threads.set(state.level)
            NotificationCenter.default.post(name: .heatLevelChanged, object: nil)
        }
        keepTicking()
        return state.level
    }

    /// While above full, a reading every few seconds lets the level come down by itself.
    private func keepTicking() {
        if state.level == .full {
            timer?.invalidate()
            timer = nil
        } else if timer == nil {
            timer = Timer.scheduledTimer(withTimeInterval: 5, repeats: true) { _ in
                MainActor.assumeIsolated { _ = HeatGovernor.shared.level }
            }
        }
    }

    /// Battery level (negative when unknown) and charging. Monitoring is switched on just to read, as it was
    /// left otherwise (live transcription manages its own).
    private static func battery() -> (Float, Bool) {
        let d = UIDevice.current
        let was = d.isBatteryMonitoringEnabled
        if !was { d.isBatteryMonitoringEnabled = true }
        let level = d.batteryLevel, state = d.batteryState
        if !was { d.isBatteryMonitoringEnabled = false }
        return (level, state == .charging || state == .full)
    }
}
