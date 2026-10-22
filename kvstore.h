#pragma once
#include <string>
#include <unordered_map>
#include <optional>
#include <mutex>
#include <queue>
#include <vector>
#include <list>
#include <chrono>
#include <functional>
#include <utility>
#include <fstream>
#include "resp.h"

// KVStore: thread-safe key-value store with TTL, LRU eviction and an append-only log.
class KVStore {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit KVStore(size_t maxKeys) : maxKeys(maxKeys) {}

    // [AOF] From now on, write every change to this file
    bool openLog(const std::string& path) {
        std::lock_guard<std::mutex> lock(mtx);
        logFile.open(path, std::ios::app | std::ios::binary);   // app = always add to the end
        return logFile.is_open();
    }

    void set(const std::string& key, const std::string& value) {
        std::lock_guard<std::mutex> lock(mtx);
        storeLocked(key, value);
        expires.erase(key);
        logLocked({"SET", key, value});
    }

    void setWithExpiry(const std::string& key, const std::string& value, long seconds) {
        std::lock_guard<std::mutex> lock(mtx);
        storeLocked(key, value);
        long long ms = seconds * 1000LL;
        addExpiryMsLocked(key, ms);
        logLocked({"SET", key, value});
        logLocked({"PEXPIREAT", key, std::to_string(unixMsNow() + ms)});   // [AOF] exact moment, not "EX 60"
    }

    std::optional<std::string> get(const std::string& key) {
        std::lock_guard<std::mutex> lock(mtx);
        if (removeIfExpiredLocked(key)) return std::nullopt;
        auto it = data.find(key);
        if (it == data.end()) return std::nullopt;
        touchLocked(key);
        return it->second;
    }

    bool del(const std::string& key) {
        std::lock_guard<std::mutex> lock(mtx);
        if (removeIfExpiredLocked(key)) return false;
        if (data.find(key) == data.end()) return false;
        eraseKeyLocked(key);
        logLocked({"DEL", key});
        return true;
    }

    bool expire(const std::string& key, long seconds) {
        std::lock_guard<std::mutex> lock(mtx);
        if (removeIfExpiredLocked(key)) return false;
        if (data.find(key) == data.end()) return false;
        long long ms = seconds * 1000LL;
        addExpiryMsLocked(key, ms);
        logLocked({"PEXPIREAT", key, std::to_string(unixMsNow() + ms)});
        return true;
    }

    // [AOF] PEXPIREAT key <unix-ms>: expire at an exact real-world moment (used when replaying the log)
    bool expireAtUnixMs(const std::string& key, long long unixMs) {
        std::lock_guard<std::mutex> lock(mtx);
        if (removeIfExpiredLocked(key)) return false;
        if (data.find(key) == data.end()) return false;
        long long msLeft = unixMs - unixMsNow();
        if (msLeft <= 0) {
            eraseKeyLocked(key);              // that moment already passed -> key is dead
        } else {
            addExpiryMsLocked(key, msLeft);
        }
        logLocked({"PEXPIREAT", key, std::to_string(unixMs)});
        return true;
    }

    long ttl(const std::string& key) {
        std::lock_guard<std::mutex> lock(mtx);
        if (removeIfExpiredLocked(key)) return -2;
        if (data.find(key) == data.end()) return -2;
        auto it = expires.find(key);
        if (it == expires.end()) return -1;
        auto msLeft = std::chrono::duration_cast<std::chrono::milliseconds>(
                          it->second - Clock::now()).count();
        return (msLeft + 999) / 1000;
    }

    size_t size() {
        std::lock_guard<std::mutex> lock(mtx);
        return data.size();
    }
    // [AOF] Hand everything waiting in the buffer to the OS in ONE go (called every 100 ms)
    void flushLog() {
        std::lock_guard<std::mutex> lock(mtx);
        if (logFile.is_open()) logFile.flush();
    }
    void removeExpired() {
        std::lock_guard<std::mutex> lock(mtx);
        TimePoint now = Clock::now();
        while (!heap.empty() && heap.top().first <= now) {
            TimePoint when = heap.top().first;
            std::string key = heap.top().second;
            heap.pop();
            auto it = expires.find(key);
            if (it != expires.end() && it->second == when) {
                eraseKeyLocked(key);
                // Not logged: the log already has PEXPIREAT, so replay deletes it on its own
            }
        }
    }

private:
    void storeLocked(const std::string& key, const std::string& value) {
        bool isNewKey = (data.find(key) == data.end());
        if (isNewKey && data.size() >= maxKeys) {
            evictLocked();
        }
        data[key] = value;
        touchLocked(key);
    }

    void touchLocked(const std::string& key) {
        auto it = lruPos.find(key);
        if (it != lruPos.end()) {
            lru.splice(lru.begin(), lru, it->second);
        } else {
            lru.push_front(key);
            lruPos[key] = lru.begin();
        }
    }

    void evictLocked() {
        if (lru.empty()) return;
        std::string victim = lru.back();
        eraseKeyLocked(victim);
        logLocked({"DEL", victim});           // [AOF] so replay evicts the same key
    }

    void eraseKeyLocked(const std::string& key) {
        data.erase(key);
        expires.erase(key);
        auto it = lruPos.find(key);
        if (it != lruPos.end()) {
            lru.erase(it->second);
            lruPos.erase(it);
        }
    }

    void addExpiryMsLocked(const std::string& key, long long ms) {
        TimePoint when = Clock::now() + std::chrono::milliseconds(ms);
        expires[key] = when;
        heap.push({when, key});
    }

    bool removeIfExpiredLocked(const std::string& key) {
        auto it = expires.find(key);
        if (it == expires.end()) return false;
        if (it->second > Clock::now()) return false;
        eraseKeyLocked(key);
        return true;
    }

    // [AOF] Real-world time in milliseconds since 1 Jan 1970 ("Unix time")
    static long long unixMsNow() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }

    // [AOF] Append one command to the log file (only if logging is switched on)
    void logLocked(const std::vector<std::string>& parts) {
        if (!logFile.is_open()) return;      // off during replay, so we don't re-log old commands
        logFile << encodeCommand(parts);     // goes into a memory buffer; flushed in batches by flushLog()
    }

    size_t maxKeys;

    std::unordered_map<std::string, std::string> data;
    std::unordered_map<std::string, TimePoint> expires;

    using Entry = std::pair<TimePoint, std::string>;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap;

    std::list<std::string> lru;
    std::unordered_map<std::string, std::list<std::string>::iterator> lruPos;

    std::ofstream logFile;                   // [AOF] the append-only file
    std::mutex mtx;
};