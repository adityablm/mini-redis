#pragma once
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>
#include "commands.h"

// Read the log file and re-run every command in it, rebuilding the store.
// Returns how many commands were replayed.
inline long replayLog(KVStore& store, const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return 0;            // no file yet = fresh start

    std::string buffer;
    char chunk[4096];
    long count = 0;
    size_t validBytes = 0;                    // bytes that formed complete commands
    bool broken = false;

    // Read the file 4 KB at a time (same pattern as recv() on the network)
    while (!broken && (file.read(chunk, sizeof(chunk)) || file.gcount() > 0)) {
        buffer.append(chunk, file.gcount());

        while (true) {
            std::vector<std::string> args;
            size_t consumed = 0;
            ParseResult result = parseCommand(buffer, args, consumed);

            if (result == ParseResult::Incomplete) break;              // need more of the file
            if (result == ParseResult::Error) { broken = true; break; } // garbage in the file

            buffer.erase(0, consumed);
            validBytes += consumed;
            handleCommand(store, args);       // re-run it (logging is OFF during replay)
            count++;
        }
    }
    file.close();

    // Leftover bytes = a half-written last command (crash mid-write) or corruption.
    // Cut them off so new commands aren't appended after garbage.
    if (broken || !buffer.empty()) {
        std::cerr << "warning: ignoring a broken command at the end of " << path << "\n";
        std::filesystem::resize_file(path, validBytes);
    }
    return count;
}