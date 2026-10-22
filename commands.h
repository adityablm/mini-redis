#pragma once
#include <string>
#include <vector>
#include <cctype>
#include "kvstore.h"
#include "resp.h"

// Make "set" and "Set" work the same as "SET"
inline std::string toUpper(std::string s) {
    for (char& c : s) c = std::toupper(static_cast<unsigned char>(c));
    return s;
}

// Turn text like "10" into a number. Returns false if it's not a positive whole number.
inline bool parsePositive(const std::string& s, long& out) {
    try {
        size_t used = 0;
        out = std::stol(s, &used);
        return used == s.size() && out > 0;   // whole text must be a number, and > 0
    } catch (...) {
        return false;
    }
}

// Turn text like "1761136260000" into a big number (can be any whole number)
inline bool parseInteger(const std::string& s, long long& out) {
    try {
        size_t used = 0;
        out = std::stoll(s, &used);
        return used == s.size();
    } catch (...) {
        return false;
    }
}

// Run ONE command and return the reply in RESP format
inline std::string handleCommand(KVStore& store, const std::vector<std::string>& args) {
    if (args.empty()) return "";
    std::string cmd = toUpper(args[0]);

    if (cmd == "PING") return simpleString("PONG");

    if (cmd == "SET") {
        // SET key value
        if (args.size() == 3) {
            store.set(args[1], args[2]);
            return simpleString("OK");
        }
        // SET key value EX seconds
        if (args.size() == 5 && toUpper(args[3]) == "EX") {
            long seconds;
            if (!parsePositive(args[4], seconds)) return errorReply("invalid expire time in 'set' command");
            store.setWithExpiry(args[1], args[2], seconds);
            return simpleString("OK");
        }
        return errorReply("syntax error");
    }

    if (cmd == "GET") {
        if (args.size() != 2) return errorReply("wrong number of arguments for 'get' command");
        auto value = store.get(args[1]);
        if (!value) return nullBulk();
        return bulkString(*value);
    }

    if (cmd == "DEL") {
        if (args.size() < 2) return errorReply("wrong number of arguments for 'del' command");
        long deleted = 0;
        for (size_t i = 1; i < args.size(); i++) {
            if (store.del(args[i])) deleted++;
        }
        return integerReply(deleted);
    }

    if (cmd == "EXPIRE") {
        if (args.size() != 3) return errorReply("wrong number of arguments for 'expire' command");
        long seconds;
        if (!parsePositive(args[2], seconds)) return errorReply("value is not an integer or out of range");
        return integerReply(store.expire(args[1], seconds) ? 1 : 0);
    }

    if (cmd == "TTL") {
        if (args.size() != 2) return errorReply("wrong number of arguments for 'ttl' command");
        return integerReply(store.ttl(args[1]));
    }

    if (cmd == "PEXPIREAT") {
        if (args.size() != 3) return errorReply("wrong number of arguments for 'pexpireat' command");
        long long at;
        if (!parseInteger(args[2], at)) return errorReply("value is not an integer or out of range");
        return integerReply(store.expireAtUnixMs(args[1], at) ? 1 : 0);
    }
    
    if (cmd == "DBSIZE") return integerReply(static_cast<long>(store.size()));
    return errorReply("unknown command '" + args[0] + "'");
}