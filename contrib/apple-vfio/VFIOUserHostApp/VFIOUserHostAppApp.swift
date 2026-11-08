//
//  VFIOUserHostAppApp.swift
//  VFIOUserHostApp
//
//  Setup app for the VFIOUserPCIDriver dext, with an explicit GPU claim action.
//  QEMU runs out-of-process and must acquire its own device claim.
//

import AppKit
import SwiftUI

/// Drives the at-launch "move to /Applications" prompt before the SwiftUI
/// window appears. Lives in the app delegate (rather than `App.init`) so we
/// have a real `NSApplication` available for `NSAlert.runModal()`.
final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationWillFinishLaunching(_ notification: Notification) {
        MoveToApplications.runIfNeeded()
    }
}

@main
struct VFIOUserHostAppApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        .windowResizability(.contentSize)
    }
}
