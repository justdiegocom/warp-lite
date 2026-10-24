import XCTest

/// The chat end to end against a real Warp, with real taps (`build.sh chat-demo <simulator id>`): pairs with the code in
/// PAIR_TICKET, shows the list, opens the pane whose tab is CHAT_TAB (a Claude Code with subagents: only looked at, never
/// typed into) and one of its subagents, then in the pane whose tab is DEMO_TAB (a Claude Code of its own) answers the
/// trust dialog with its button, sends REPLY, answers the question it asks with a button and taps the first quick reply
/// its answer offers. Screenshots go to SHOTS (a folder). Skipped without PAIR_TICKET.
final class ChatDemoTests: XCTestCase {
    private let env = ProcessInfo.processInfo.environment

    private func pause(_ s: Double) { Thread.sleep(forTimeInterval: s) }

    private func shot(_ name: String) {
        guard let dir = env["SHOTS"], !dir.isEmpty else { return }
        let png = XCUIScreen.main.screenshot().pngRepresentation
        try? png.write(to: URL(fileURLWithPath: dir).appendingPathComponent("m90-\(name).png"))
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

    private func option(_ app: XCUIApplication, _ label: String) -> XCUIElement {
        app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH 'option-' AND label CONTAINS[c] %@", label)).firstMatch
    }

    private func back(_ app: XCUIApplication) {
        app.navigationBars.buttons.element(boundBy: 0).tap()
        pause(1.5)
    }

    func testChatDemo() throws {
        guard let ticket = env["PAIR_TICKET"], !ticket.isEmpty else { throw XCTSkip("PAIR_TICKET not set") }
        let app = XCUIApplication()
        app.launchArguments += ["-pairTicket", ticket]
        app.launch()

        // The list: what each agent does, unread badges.
        let chat = row(app, tab: env["CHAT_TAB"] ?? "fnviewer")
        XCTAssertTrue(chat.waitForExistence(timeout: 90), "no pane with that tab")
        pause(6)
        shot("1-list")
        chat.tap()

        // Its conversation as a chat, with its subagents (chips at the top, cards in the chat).
        let chip = app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH 'agent-chip-a'")).firstMatch
        let card = app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH 'agent-card-a'")).firstMatch
        XCTAssertTrue(wait(90) { chip.exists || card.exists }, "no subagent")
        pause(3)
        shot("2-chat")
        // Older messages: a swipe on one of them (queries by identifier only: a long chat makes a whole snapshot slow).
        let bubble = app.descendants(matching: .any).matching(identifier: "item-assistant").firstMatch
        if bubble.exists && bubble.isHittable {
            bubble.swipeDown()
            pause(2)
            shot("3-earlier")
            for _ in 0..<2 where bubble.exists && bubble.isHittable {
                bubble.swipeUp()
            }
            pause(1)
        }
        // One subagent's own conversation.
        let open = chip.exists && chip.isHittable ? chip : card
        XCTAssertTrue(open.isHittable, "no subagent to open")
        open.tap()
        XCTAssertTrue(app.descendants(matching: .any).matching(identifier: "item-tool").firstMatch.waitForExistence(timeout: 60)
                      || app.descendants(matching: .any).matching(identifier: "tool-group").firstMatch.waitForExistence(timeout: 5),
                      "the subagent's chat is empty")
        pause(3)
        shot("4-subagent")
        back(app)
        back(app)

        // A Claude Code of our own: its trust dialog, a question and a quick reply, with buttons.
        let demo = row(app, tab: env["DEMO_TAB"] ?? "demo")
        XCTAssertTrue(demo.waitForExistence(timeout: 20))
        demo.tap()
        let trust = option(app, "trust")
        if trust.waitForExistence(timeout: 40) {
            XCTAssertTrue(wait(30) { trust.isEnabled }, "typing is off for this phone")
            pause(2)
            shot("5-trust")
            trust.tap()
        }
        let reply = app.descendants(matching: .any).matching(identifier: "reply").firstMatch
        XCTAssertTrue(wait(60) { reply.exists && reply.isEnabled && self.status(app) == "Finished" }, "not ready: \(status(app))")
        reply.tap()
        reply.typeText(env["REPLY"] ?? "Use the AskUserQuestion tool to ask me which color I like (red, green or blue). Then reply with only three lines: the line «Next steps:», then «1. Write a haiku about my color», then «2. Say goodbye».")
        pause(1)
        app.buttons["reply-send"].tap()
        let green = option(app, env["PICK"] ?? "green")
        XCTAssertTrue(green.waitForExistence(timeout: 120), "no question")
        pause(3)
        shot("6-question")
        green.tap()
        // A permission it may ask on the way.
        for _ in 0..<2 {
            let yes = app.buttons.matching(NSPredicate(format: "identifier BEGINSWITH 'option-' AND label BEGINSWITH 'Yes'")).firstMatch
            guard yes.waitForExistence(timeout: 6) else { break }
            yes.tap()
            pause(2)
        }
        let quick = app.buttons["quick-reply-0"]
        XCTAssertTrue(quick.waitForExistence(timeout: 120), "no quick reply")
        pause(3)
        shot("7-quick-reply")
        quick.tap()
        wait(20) { self.status(app) != "Finished" }
        XCTAssertTrue(wait(120) { self.status(app) == "Finished" }, "no answer: \(status(app))")
        pause(3)
        shot("8-answer")
        back(app)
        pause(3)
        shot("9-list")
    }
}
