import XCTest

/// A remembered computer that is gone, with no Warp needed: the app is launched remembering one that never answers.
/// Forget and Pair again go back to the pairing screen at once, whatever the connection is doing. SHOTS (a folder) gets
/// a screenshot of the «Can't reach» card. It replaces the app's computer, so it runs in a Simulator of its own and not
/// in build.sh demo: build.sh's xcodebuild arguments with -destination id=<that Simulator>
/// -only-testing:WarpRemoteUITests/ForgetTests test.
final class ForgetTests: XCTestCase {
    /// A pairing code nobody answers: endpoint id Ed25519's base point (a valid key no Warp uses), secret zeros, and one
    /// address, 192.0.2.1:9 (reserved for documentation: nothing there, so a connect waits its full timeout).
    private let gone = "WARP1AFMGMZTGMZTGMZTGMZTGMZTGMZTGMZTGMZTGMZTGMZTGMZTGMZTGMAAAAAAAAAAAAAAAAAAAAAAAAAABAQDMAAACAEAAS"
    private let env = ProcessInfo.processInfo.environment

    private func launch() -> XCUIApplication {
        let app = XCUIApplication()
        app.launchArguments += ["-rememberTicket", gone, "-rememberName", "Old Warp"]
        app.launch()
        return app
    }

    private func pairingScreen(_ app: XCUIApplication) -> XCUIElement {
        app.navigationBars["Pair with Warp"]
    }

    /// Forget while the app is still trying to reach it: the pairing screen within a second.
    func testForgetWhileConnecting() {
        let app = launch()
        let connection = app.staticTexts["connection"]
        XCTAssertTrue(connection.waitForExistence(timeout: 10), "no connection row")
        XCTAssertEqual(connection.label, "Connecting…")
        app.buttons["more"].tap()
        app.buttons["Forget this computer"].tap()
        let forget = app.buttons["Forget"]
        XCTAssertTrue(forget.waitForExistence(timeout: 5), "no Forget confirmation")
        forget.tap()
        XCTAssertTrue(pairingScreen(app).waitForExistence(timeout: 1), "the pairing screen didn't show within 1 s")
        XCTAssertFalse(app.staticTexts["connection"].exists)
    }

    /// It doesn't answer twice in a row: «Can't reach» with Try again and Pair again, and Pair again while it tries
    /// again goes to the pairing screen within a second.
    func testCantReach() {
        let app = launch()
        let card = app.staticTexts["unreachable"]
        XCTAssertTrue(card.waitForExistence(timeout: 120), "no Can't reach card")
        XCTAssertEqual(card.label, "Can't reach Old Warp")
        if let dir = env["SHOTS"], !dir.isEmpty {
            let png = XCUIScreen.main.screenshot().pngRepresentation
            try? png.write(to: URL(fileURLWithPath: dir).appendingPathComponent("m88-cant-reach.png"))
        }
        let again = app.buttons["try-again"]
        XCTAssertTrue(again.isEnabled)
        again.tap()
        XCTAssertFalse(again.isEnabled, "Try again should show it is trying")
        XCTAssertTrue(card.exists, "the card stays while it tries again")
        app.buttons["pair-again"].tap()
        XCTAssertTrue(pairingScreen(app).waitForExistence(timeout: 1), "the pairing screen didn't show within 1 s")
    }
}
