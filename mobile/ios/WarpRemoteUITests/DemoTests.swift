import XCTest

/// The app end to end against a real Warp, with real taps (`build.sh demo`): pairs with the code in PAIR_TICKET, opens
/// the pane whose tab is CHAT_TAB, answers each question the agent asks with its «Yes» button, sends REPLY and waits
/// for the answer, then types SHELL_CMD into the pane whose tab is SHELL_TAB. Screenshots go to SHOTS (a folder).
/// Skipped without PAIR_TICKET.
final class DemoTests: XCTestCase {
    private let env = ProcessInfo.processInfo.environment

    private func pause(_ s: Double) { Thread.sleep(forTimeInterval: s) }

    private func shot(_ name: String) {
        guard let dir = env["SHOTS"], !dir.isEmpty else { return }
        let png = XCUIScreen.main.screenshot().pngRepresentation
        try? png.write(to: URL(fileURLWithPath: dir).appendingPathComponent("m2-\(name).png"))
    }

    private func status(_ app: XCUIApplication) -> String {
        let s = app.descendants(matching: .any).matching(identifier: "status").firstMatch
        return s.exists ? s.label : ""
    }

    @discardableResult
    private func wait(_ timeout: Double, _ ok: () -> Bool) -> Bool {
        let end = Date().addingTimeInterval(timeout)
        while Date() < end {
            if ok() { return true }
            pause(0.5)
        }
        return false
    }

    private func row(_ app: XCUIApplication, tab: String) -> XCUIElement {
        app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH 'session-' AND label CONTAINS %@", tab)).firstMatch
    }

    private func type(_ app: XCUIApplication, _ text: String) {
        let reply = app.descendants(matching: .any).matching(identifier: "reply").firstMatch
        XCTAssertTrue(reply.waitForExistence(timeout: 10))
        XCTAssertTrue(wait(30) { reply.isEnabled }, "typing is off for this phone")
        reply.tap()
        reply.typeText(text)
        pause(1)
        app.buttons["reply-send"].tap()
    }

    func testDemo() throws {
        guard let ticket = env["PAIR_TICKET"], !ticket.isEmpty else { throw XCTSkip("PAIR_TICKET not set") }
        let app = XCUIApplication()
        app.launchArguments += ["-pairTicket", ticket]
        app.launch()

        // The terminals of the computer.
        let chat = row(app, tab: env["CHAT_TAB"] ?? "Claude")
        XCTAssertTrue(chat.waitForExistence(timeout: 90), "no pane with that tab")
        pause(3)
        shot("1-terminals")
        chat.tap()

        // Each question the agent asks: a moment to read it, then «Yes».
        var answered = 0
        while answered < 4 {
            let yes = app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH 'option-' AND label BEGINSWITH 'Yes'")).firstMatch
            guard yes.waitForExistence(timeout: answered == 0 ? 90 : 25) else { break }
            XCTAssertTrue(wait(30) { yes.isEnabled }, "typing is off for this phone")
            pause(3)
            if answered == 0 { shot("2-question") }
            yes.tap()
            answered += 1
            pause(2)
        }
        XCTAssertGreaterThan(answered, 0, "no question to answer")
        XCTAssertTrue(wait(120) { status(app) == "Finished" }, "not finished: \(status(app))")
        pause(2)
        shot("3-chat")

        // A reply from the phone, and the agent's answer.
        type(app, env["REPLY"] ?? "Thanks! Now answer in one short sentence: what did you just do?")
        wait(20) { status(app) != "Finished" }
        XCTAssertTrue(wait(120) { status(app) == "Finished" }, "no answer: \(status(app))")
        pause(3)
        shot("4-answer")

        // A plain shell: its screen as text, typed into from the phone.
        if let tab = env["SHELL_TAB"], !tab.isEmpty {
            app.navigationBars.buttons.element(boundBy: 0).tap()
            let shell = row(app, tab: tab)
            XCTAssertTrue(shell.waitForExistence(timeout: 20))
            pause(1)
            shell.tap()
            pause(2)
            type(app, env["SHELL_CMD"] ?? "Get-Date")
            pause(4)
            shot("5-shell")
        }
    }
}
