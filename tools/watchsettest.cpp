// watchsettest: the Auto columns watcher's bookkeeping under re-entry.
//
// Regression test for the 0.99.1 explorer.exe heap corruption: in Explorer,
// IConnectionPoint::Unadvise pumps messages and a ShellWindows event re-enters
// the watcher's "attach all" pass, which removes the same window again. Here
// the release callback plays Unadvise and re-enters the set the same ways.
// Every value owns heap memory, so a double free or a use after free shows up
// under ASan (cmake -DPSE_ASAN=ON) or page heap; without them the counts
// still catch double releases. Exit code 0 = pass.
#include <cstdio>
#include <memory>
#include <set>

#include "watch_set.h"

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

struct Conn {
    std::unique_ptr<int> id;  // heap memory, like the map node's ComPtr target
};

using Set = pse::WatchSet<int, Conn>;

// The 0.99.1 crash: Remove(k) (OnQuit) is unadvising k when the nested pass
// (WindowRevoked) drops every window that is no longer listed, k included.
void RemoveReentersRemoveIf() {
    std::multiset<int> released;
    Set* self = nullptr;
    Set s([&](Conn& c) {
        released.insert(*c.id);
        if (*c.id == 1) self->RemoveIf([](int k) { return k != 3; });  // only 3 still listed
    });
    self = &s;
    for (int k : {1, 2, 3}) s.Add(k, {std::make_unique<int>(k)});
    s.Remove(1);
    Check(released.count(1) == 1, "remove+removeIf: 1 released once");
    Check(released.count(2) == 1, "remove+removeIf: 2 released once");
    Check(!s.Contains(1) && !s.Contains(2) && s.Contains(3), "remove+removeIf: only 3 left");
}

// The same window closing twice over: Remove(k) re-entered by Remove(k).
void RemoveReentersRemove() {
    int releases = 0;
    Set* self = nullptr;
    Set s([&](Conn& c) {
        ++releases;
        if (*c.id == 7) self->Remove(7);
    });
    self = &s;
    s.Add(7, {std::make_unique<int>(7)});
    s.Remove(7);
    Check(releases == 1, "remove+remove: released once");
    Check(s.Size() == 0, "remove+remove: empty");
}

// The pass's own sweep re-entered: releasing one dropped window runs the
// sweep again (a second WindowRevoked).
void RemoveIfReentersRemoveIf() {
    std::multiset<int> released;
    Set* self = nullptr;
    Set s([&](Conn& c) {
        released.insert(*c.id);
        self->RemoveIf([](int k) { return k % 2 == 0; });
    });
    self = &s;
    for (int k = 0; k < 10; ++k) s.Add(k, {std::make_unique<int>(k)});
    s.RemoveIf([](int k) { return k < 5; });
    for (int k = 0; k < 10; ++k) {
        const bool gone = k < 5 || k % 2 == 0;
        Check(released.count(k) == (gone ? 1u : 0u), "removeIf+removeIf: released once each");
        Check(s.Contains(k) == !gone, "removeIf+removeIf: membership");
    }
}

// Shutdown: Clear() while events still arrive and add or remove windows.
void ClearReentersAddAndRemove() {
    std::multiset<int> released;
    Set* self = nullptr;
    bool added = false;
    Set s([&](Conn& c) {
        released.insert(*c.id);
        self->Remove(2);
        if (!added) {
            added = true;
            self->Add(9, {std::make_unique<int>(9)});
        }
    });
    self = &s;
    for (int k : {1, 2, 3}) s.Add(k, {std::make_unique<int>(k)});
    s.Clear();
    for (int k : {1, 2, 3}) Check(released.count(k) == 1, "clear: each released once");
    Check(s.Contains(9) && s.Size() == 1, "clear: the window added during it stays");
    s.Clear();
    Check(released.count(9) == 1 && s.Size() == 0, "clear: second clear releases it");
}

// A window attached by a nested pass while the outer pass's Advise ran.
void AddWhenPresentReleasesTheNewValue() {
    int releases = 0;
    Set s([&](Conn&) { ++releases; });
    Check(s.Add(4, {std::make_unique<int>(4)}), "add: first add");
    Check(!s.Add(4, {std::make_unique<int>(4)}), "add: second add refused");
    Check(releases == 1 && s.Size() == 1, "add: the refused value released, the first kept");
}

void SerialPassNeverNests() {
    pse::SerialPass pass;
    int depth = 0, maxDepth = 0, runs = 0, calls = 0;
    std::function<void()> run;
    run = [&] {
        pass.Run([&] {
            ++depth;
            ++runs;
            if (depth > maxDepth) maxDepth = depth;
            if (calls++ < 3) {
                run();  // an event arriving inside the pass
                run();
            }
            --depth;
        });
    };
    run();
    Check(maxDepth == 1, "pass: never nested");
    Check(runs == 4, "pass: re-requested runs happen afterwards, coalesced");
    Check(!pass.Running(), "pass: not running at the end");
    pass.Stop();
    int after = 0;
    pass.Run([&] { ++after; });
    Check(after == 0, "pass: nothing runs after Stop");
}

}  // namespace

int main() {
    RemoveReentersRemoveIf();
    RemoveReentersRemove();
    RemoveIfReentersRemoveIf();
    ClearReentersAddAndRemove();
    AddWhenPresentReleasesTheNewValue();
    SerialPassNeverNests();
    if (g_failures) {
        std::printf("watchsettest: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("watchsettest: all passed\n");
    return 0;
}
