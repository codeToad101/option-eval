// wake_trigger.swift
// ------------------
// Tiny macOS app that listens for system wake notifications and fires
// catchup_runner.sh. Compile once and add to Login Items.
//
// Compile:
//   swiftc wake_trigger.swift -o wake_trigger \
//       -framework Cocoa -framework Foundation
//
// Run at login:
//   System Settings → General → Login Items → add wake_trigger
//
// Or just use sleepwatcher (homebrew) as an alternative — see README.
//
// This is the simplest possible approach: no UI, no dock icon,
// just a background process that wakes up when the system does.

import Cocoa
import Foundation

// EDIT_ME: absolute path to catchup_runner.sh
let runnerPath = "EDIT_ME/python/catchup_runner.sh"

func runCatchup() {
    let task = Process()
    task.executableURL = URL(fileURLWithPath: "/bin/bash")
    task.arguments = [runnerPath]
    task.standardOutput = FileHandle.nullDevice
    task.standardError  = FileHandle.nullDevice
    do {
        try task.run()
    } catch {
        NSLog("wake_trigger: failed to launch catchup_runner: \(error)")
    }
}

// Register for wake notification
NSWorkspace.shared.notificationCenter.addObserver(
    forName: NSWorkspace.didWakeNotification,
    object: nil,
    queue: .main
) { _ in
    NSLog("wake_trigger: system woke — firing catchup_runner.sh")
    runCatchup()
}

NSLog("wake_trigger: listening for wake events")
NSApplication.shared.run()  // keep process alive