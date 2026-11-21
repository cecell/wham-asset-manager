// The Auto columns watcher's bookkeeping, safe against re-entry.
//
// The watcher thread is a COM single-threaded apartment. Every outgoing COM
// call it makes into another apartment (IConnectionPoint::Unadvise/Advise,
// IShellWindows::Item, ...) runs a modal message loop until the call returns,
// and that loop dispatches incoming calls: a ShellWindows WindowRevoked event
// can run the "attach all" pass in the middle of an Unadvise. Up to 0.99.1 the
// watcher held a map iterator across Unadvise, the nested pass erased the same
// entry, and the outer code then erased the freed node again: heap corruption
// in explorer.exe (c0000374, 2026-11-17 to 11-20).
//
// The rules here: an entry leaves the map BEFORE its release call runs, so
// nothing re-entrant can see or free it again; and a pass that is already
// running is not started a second time, it runs once more when it ends.
#pragma once

#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace pse {

template <class Key, class Value>
class WatchSet {
public:
    // Called for every value that leaves the set. It may re-enter the set.
    using Release = std::function<void(Value&)>;

    explicit WatchSet(Release release) : release_(std::move(release)) {}
    WatchSet(const WatchSet&) = delete;
    WatchSet& operator=(const WatchSet&) = delete;
    ~WatchSet() { Clear(); }

    bool Contains(const Key& k) const { return map_.count(k) != 0; }
    size_t Size() const { return map_.size(); }

    // Adds k. When k is already there (a re-entrant pass got there first) the
    // new value is released instead and false is returned.
    bool Add(const Key& k, Value v) {
        if (map_.count(k)) {
            release_(v);
            return false;
        }
        map_.emplace(k, std::move(v));
        return true;
    }

    // Removes k, if present.
    void Remove(const Key& k) {
        auto node = map_.extract(k);
        if (node) release_(node.mapped());
    }

    // Removes every entry for which drop(key) is true. drop must not call COM.
    template <class Pred>
    void RemoveIf(Pred drop) {
        std::vector<Value> out;
        for (auto it = map_.begin(); it != map_.end();) {
            if (drop(it->first)) {
                out.push_back(std::move(it->second));
                it = map_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto& v : out) release_(v);
    }

    void Clear() {
        std::map<Key, Value> all;
        all.swap(map_);
        for (auto& [k, v] : all) release_(v);
    }

private:
    Release release_;
    std::map<Key, Value> map_;
};

// Runs a pass so that it never nests: a call made while the pass is running
// only asks for one more run after it, and stop() ends the repeats.
class SerialPass {
public:
    template <class F>
    void Run(F&& pass) {
        if (running_) {
            again_ = true;
            return;
        }
        running_ = true;
        do {
            again_ = false;
            if (stopped_) break;
            pass();
        } while (again_);
        running_ = false;
    }
    void Stop() { stopped_ = true; }
    bool Running() const { return running_; }

private:
    bool running_ = false, again_ = false, stopped_ = false;
};

}  // namespace pse
