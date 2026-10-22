#include <iostream>
#include <chrono>
#include <string>
#include <vector>
#include <thread>          // std::thread
#include <functional>      // std::ref
#include <csignal>         // signal, SIGPIPE
#include <cstdio>          // perror
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "aof.h"

const int PORT = 6380;

// Each worker thread runs this: serve ONE client until they disconnect
void handleClient(int client_fd, KVStore& store) {
    std::string buffer;
    char chunk[1024];

    while (true) {
        ssize_t n = recv(client_fd, chunk, sizeof(chunk), 0);
        if (n <= 0) break;                       // client hung up or error
        buffer.append(chunk, n);

        while (true) {
            std::vector<std::string> args;
            size_t consumed = 0;
            ParseResult result = parseCommand(buffer, args, consumed);

            if (result == ParseResult::Incomplete) break;

            if (result == ParseResult::Error) {
                std::string err = errorReply("protocol error");
                send(client_fd, err.data(), err.size(), 0);
                buffer.clear();
                break;
            }

            buffer.erase(0, consumed);
            std::string reply = handleCommand(store, args);
            if (!reply.empty()) send(client_fd, reply.data(), reply.size(), 0);
        }
    }

    close(client_fd);
}
// Background worker: every 100 ms, delete keys whose time is up
void cleanerLoop(KVStore& store) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        store.removeExpired();
        store.flushLog();
    }
}

int main(int argc, char* argv[]) {
    // If a client disconnects while we're replying, Linux would normally
    // KILL our whole server (a "SIGPIPE" signal). This line says: ignore it.
    signal(SIGPIPE, SIG_IGN);

    // Optional limit: "./server 3" allows at most 3 keys (default 100000)
    size_t maxKeys = 100000;
    if (argc > 1) {
        long n;
        if (!parsePositive(argv[1], n)) {
            std::cerr << "usage: ./server [maxKeys]\n";
            return 1;
        }
        maxKeys = n;
    }
    KVStore store(maxKeys);   // ONE store, shared by ALL threads
    std::cout << "max keys: " << maxKeys << "\n";
    // [AOF] 1) Rebuild data from the log, 2) THEN switch logging on
    const std::string AOF_PATH = "appendonly.aof";
    long replayed = replayLog(store, AOF_PATH);
    std::cout << "replayed " << replayed << " commands from " << AOF_PATH << "\n";
    if (!store.openLog(AOF_PATH)) {
        std::cerr << "could not open " << AOF_PATH << "\n";
        return 1;
    }
    std::thread cleaner(cleanerLoop, std::ref(store));   // start the cleaner thread
    cleaner.detach();

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);
    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }

    if (listen(server_fd, 128) < 0) { perror("listen"); return 1; }
    std::cout << "mini-redis listening on port " << PORT << "\n";

    // The main thread ONLY answers the door
    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd < 0) { perror("accept"); continue; }

        // Hire a worker thread for this client.
        // std::ref(store) = give the thread the REAL store, not a copy.
        std::thread worker(handleClient, client_fd, std::ref(store));
        worker.detach();   // let it run on its own; main goes back to accept()
    }
}