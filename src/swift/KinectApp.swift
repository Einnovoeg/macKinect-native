import SwiftUI
import AppKit

struct KinectAppUI: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
                .navigationTitle("macKinect")
        }
        .windowStyle(.titleBar)
    }
}
