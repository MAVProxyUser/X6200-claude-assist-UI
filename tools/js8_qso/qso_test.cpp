// Host test of the JS8 QSO book (src/js8/qso_book.cpp, QsoTracker): when a two-way QSO ends it is offered for the
// log once, and a later exchange with the same station is a QSO of its own, offered again (2026-11-26: the bench's
// js8-look ran its two directions three minutes apart, and the second's QSO was still the first's, already offered:
// no popup at its 73). An answer that closes too ('73' back after ours) stays in the QSO offered, its reports kept for
// Log QSO later. `make` builds and runs it.
#include "qso_book.hpp"

#include <cstdio>
#include <optional>
#include <string>

using x6100::js8::QsoTracker;

static int fails, checks;

static void check(bool ok, const char *what) {
    checks++;
    if (!ok) fails++;
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
}

static const std::string ME = "Q1TST", HIM = "Q2TST";
static const std::int64_t MIN = 60 * 1000;

int main() {
    QsoTracker q;
    std::int64_t t = 1000000;
    std::optional<std::string> got;

    // A QSO: he calls us, we answer, he closes with 73: offered at his 73
    got = q.received(HIM, HIM + ": " + ME + " TESTWORD HELLO", true, -5, ME, t);
    check(!got, "his first message offers nothing");
    got = q.sent(HIM + " HELLO", ME, t += MIN);
    check(!got, "our answer offers nothing (not closing)");
    got = q.received(HIM, HIM + ": " + ME + " 73", true, -5, ME, t += MIN);
    check(got && *got == HIM, "his 73 closes the QSO: offered");

    // Our 73 back: the same QSO, ended and offered already: nothing more
    got = q.sent(HIM + " 73", ME, t += MIN / 2);
    check(!got, "our 73 after his: no second offer of the same QSO");
    got = q.received(HIM, HIM + ": " + ME + " 73", true, -5, ME, t += MIN / 2);
    check(!got, "his 73 again: no second offer");
    check(q.get(HIM, t).has_value(), "the QSO still there for Log QSO after the 73s");

    // Three minutes later, well within EXPIRE_MS: a new exchange is a QSO of its own, offered at its end
    got = q.received(HIM, HIM + ": " + ME + " TESTWORD HELLO", true, -5, ME, t += 3 * MIN);
    check(!got, "a new message after the QSO ended: nothing yet");
    got = q.sent(HIM + " HELLO", ME, t += MIN);
    check(!got, "our answer: nothing yet");
    got = q.received(HIM, HIM + ": " + ME + " 73", true, -5, ME, t += MIN);
    check(got && *got == HIM, "the second QSO's 73: offered again");

    // Closed from our side: he calls, we answer, we close with 73: offered at ours
    QsoTracker r;
    t = 5000000;
    r.received(HIM, HIM + ": " + ME + " HELLO", true, -5, ME, t);
    got = r.sent(HIM + " 73", ME, t += MIN);
    check(got && *got == HIM, "our 73 closes a two-way QSO: offered");

    // One side only: never offered
    QsoTracker s;
    t = 9000000;
    got = s.received(HIM, HIM + ": " + ME + " 73", true, -5, ME, t);
    check(!got, "his 73 with nothing from us: not a QSO");

    std::printf("JS8 QSO BOOK %s: %d of %d cases\n", fails ? "FAILED" : "OK", checks - fails, checks);
    return fails ? 1 : 0;
}
