#pragma once
#include <string>
#include <vector>
#include <sstream>

// ===================== WRITING REPLIES =====================

inline std::string simpleString(const std::string& s) { return "+" + s + "\r\n"; }       // +OK
inline std::string errorReply(const std::string& msg) { return "-ERR " + msg + "\r\n"; }  // -ERR ...
inline std::string integerReply(long n) { return ":" + std::to_string(n) + "\r\n"; }      // :1
inline std::string nullBulk() { return "$-1\r\n"; }                                       // (nil)

// $6\r\nLokesh\r\n  -> length first, then the value
inline std::string bulkString(const std::string& s) {
    return "$" + std::to_string(s.size()) + "\r\n" + s + "\r\n";
}
// Turn {"SET","name","Lokesh"} into *3\r\n$3\r\nSET\r\n$4\r\nname\r\n$6\r\nLokesh\r\n
inline std::string encodeCommand(const std::vector<std::string>& parts) {
    std::string out = "*" + std::to_string(parts.size()) + "\r\n";
    for (const auto& p : parts) out += bulkString(p);
    return out;
}

// ===================== READING COMMANDS =====================

enum class ParseResult { Complete, Incomplete, Error };

// Split "SET name Lokesh" into {"SET", "name", "Lokesh"} (used for plain-text commands)
inline std::vector<std::string> splitWords(const std::string& line) {
    std::vector<std::string> words;
    std::istringstream stream(line);
    std::string word;
    while (stream >> word) words.push_back(word);
    return words;
}

// Reads a line like "*3\r\n" or "$6\r\n" starting at position pos.
// Checks the first character is `prefix` ('*' or '$'), stores the number in `out`,
// and moves pos to just after the "\r\n".
inline ParseResult readNumberLine(const std::string& buf, size_t& pos, char prefix, long& out) {
    if (pos >= buf.size()) return ParseResult::Incomplete;
    if (buf[pos] != prefix) return ParseResult::Error;

    size_t end = buf.find("\r\n", pos);
    if (end == std::string::npos) return ParseResult::Incomplete;   // line not finished yet

    try {
        out = std::stol(buf.substr(pos + 1, end - pos - 1));      // the digits between prefix and \r\n
    } catch (...) {
        return ParseResult::Error;                                // not a number
    }
    pos = end + 2;   // skip past "\r\n"
    return ParseResult::Complete;
}

// Tries to read ONE command from the start of buf.
//   Complete   -> args is filled, consumed = number of bytes used
//   Incomplete -> not all bytes have arrived yet; wait for more
//   Error      -> the data is broken
inline ParseResult parseCommand(const std::string& buf, std::vector<std::string>& args, size_t& consumed) {
    args.clear();
    if (buf.empty()) return ParseResult::Incomplete;

    // Plain-text command (e.g. typed in nc): "SET name Lokesh\n"
    if (buf[0] != '*') {
        size_t end = buf.find('\n');
        if (end == std::string::npos) return ParseResult::Incomplete;
        std::string line = buf.substr(0, end);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        args = splitWords(line);
        consumed = end + 1;
        return ParseResult::Complete;
    }

    // RESP command: *<count>\r\n followed by <count> items of $<len>\r\n<value>\r\n
    size_t pos = 0;
    long count;
    ParseResult r = readNumberLine(buf, pos, '*', count);
    if (r != ParseResult::Complete) return r;

    for (long i = 0; i < count; i++) {
        long len;
        r = readNumberLine(buf, pos, '$', len);
        if (r != ParseResult::Complete) return r;
        if (len < 0) return ParseResult::Error;

        if (buf.size() < pos + len + 2) return ParseResult::Incomplete;  // value + \r\n not fully here
        args.push_back(buf.substr(pos, len));   // take exactly `len` characters
        pos += len + 2;                         // skip the value and its \r\n
    }

    consumed = pos;
    return ParseResult::Complete;
}