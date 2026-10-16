// snek worker <coordinator[:port]> [threads] [name]
// Distributed solving: connects to the coordinator (dist/coordinator.py), receives positions,
// solves them exactly and sends back the values.
//
// Line protocol over TCP:
//   worker -> coordinator:  hello <name> <threads>
//                           get <n>                                  ask for up to n units
//                           result <id> <value> <nodes> <seconds>    (value -9: node budget exceeded)
//                           result <id> bad 0 0                      (unit had an illegal move)
//   coordinator -> worker:  unit <id> [<node budget>] <move> <move> ...   0..n of these, then
//                           end
// With env SOLVER_WIN_TEST=1 the worker only asks whether the side to move wins (cheaper): a unit
// that is not a win comes back as 0 (draw or loss) or -1 (proven loss) instead of its exact value.
// A worker that loses the connection reconnects; the coordinator re-queues its units.
// The threads share one transposition table, kept from unit to unit (makeSweepTable); the rest of a
// solver's state is reset for every unit (Solver::newPosition).
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
#include "commands.hpp"
#include "notation.hpp"
#include "solver.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using Socket = SOCKET;
static const Socket NO_SOCKET = INVALID_SOCKET;
static void closeSocket(Socket s) { closesocket(s); }
#else
#include <csignal>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>
using Socket = int;
static const Socket NO_SOCKET = -1;
static void closeSocket(Socket s) { close(s); }
#endif

// A TCP connection that sends and receives whole lines.
class LineConnection {
  public:
    bool connectTo(const std::string& host, const std::string& port) {
        addrinfo hints{}, *address = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &address) != 0 || !address) return false;
        socket_ = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        bool ok = socket_ != NO_SOCKET && connect(socket_, address->ai_addr, (int)address->ai_addrlen) == 0;
        freeaddrinfo(address);
        if (!ok) {
            if (socket_ != NO_SOCKET) closeSocket(socket_);
            socket_ = NO_SOCKET;
            return false;
        }
        received.clear();
        return true;
    }

    // Thread-safe (solver threads send their results directly).
    bool sendLine(const std::string& line) {
        std::lock_guard<std::mutex> lock(sendLock);
        if (socket_ == NO_SOCKET) return false;
        std::string data = line + "\n";
        size_t sent = 0;
        while (sent < data.size()) {
            int n = send(socket_, data.data() + sent, (int)(data.size() - sent), 0);
            if (n <= 0) return false;
            sent += n;
        }
        return true;
    }

    bool readLine(std::string& line) {
        for (;;) {
            size_t end = received.find('\n');
            if (end != std::string::npos) {
                line = received.substr(0, end);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                received.erase(0, end + 1);
                return true;
            }
            char chunk[4096];
            int n = recv(socket_, chunk, sizeof chunk, 0);
            if (n <= 0) return false;
            received.append(chunk, n);
        }
    }

    void close() {
        std::lock_guard<std::mutex> lock(sendLock);
        if (socket_ != NO_SOCKET) closeSocket(socket_);
        socket_ = NO_SOCKET;
    }

  private:
    Socket socket_ = NO_SOCKET;
    std::string received;
    std::mutex sendLock;
};

struct Unit {
    std::string id;
    std::string body;  // [<node budget>] <move> <move> ...
};

// Solves one unit and writes the result line for the coordinator; false if the unit was invalid.
// winTest: only whether the side to move wins (the window (0, 1)).
static bool solveUnit(const Unit& unit, Solver& solver, bool winTest, std::string& resultLine) {
    Position pos = Position::initial();
    std::istringstream words(unit.body);
    std::string word;
    uint64_t nodeBudget = 0;  // 0 = unlimited; a leading bare number is the budget
    bool first = true, ok = true;
    while (words >> word) {
        bool isBudget = first && word.find(':') == std::string::npos && word != "pass";
        first = false;
        if (isBudget) { nodeBudget = std::strtoull(word.c_str(), nullptr, 10); continue; }
        if (!playMoveText(pos, word)) { ok = false; break; }
    }
    char line[256];
    if (!ok) {
        snprintf(line, sizeof line, "result %s bad 0 0", unit.id.c_str());
        resultLine = line;
        return false;
    }
    solver.newPosition();
    auto start = std::chrono::steady_clock::now();
    int value = solver.solve(pos, 0, nodeBudget ? nodeBudget : UINT64_MAX, winTest ? 0 : -1, 1);  // -9: budget exceeded
    double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    snprintf(line, sizeof line, "result %s %d %llu %.2f", unit.id.c_str(), value, (unsigned long long)solver.nodes, seconds);
    resultLine = line;
    return true;
}

static int runWorker(const std::string& hostAndPort, int threads, const std::string& name) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    signal(SIGPIPE, SIG_IGN);  // a dropped connection must not kill the worker
#endif
    size_t colon = hostAndPort.rfind(':');
    std::string host = colon == std::string::npos ? hostAndPort : hostAndPort.substr(0, colon);
    std::string port = colon == std::string::npos ? "8090" : hostAndPort.substr(colon + 1);

    LineConnection connection;
    std::mutex queueLock;
    std::condition_variable queueChanged;
    std::deque<Unit> queue;
    int busy = 0;  // units being solved right now
    std::atomic<uint64_t> solvedCount{0};
    auto startTime = std::chrono::steady_clock::now();

    // solver threads: take a unit, solve it, send the result
    std::vector<std::thread> solvers;
    std::shared_ptr<TranspositionTable> table = makeSweepTable();  // the units are usually siblings
    for (int w = 0; w < threads; w++)
        solvers.emplace_back([&] {
            Solver solver(table);
            bool winTest = envInt("SOLVER_WIN_TEST", 0) != 0;
            for (;;) {
                Unit unit;
                {
                    std::unique_lock<std::mutex> lock(queueLock);
                    queueChanged.wait(lock, [&] { return !queue.empty(); });
                    unit = queue.front();
                    queue.pop_front();
                    busy++;
                }
                std::string result;
                if (solveUnit(unit, solver, winTest, result)) solvedCount++;
                connection.sendLine(result);
                std::lock_guard<std::mutex> lock(queueLock);
                busy--;
            }
        });

    // main thread: keep the local queue filled (threads + 2 units in hand)
    printf("worker '%s': %d threads, coordinator %s:%s\n", name.c_str(), threads, host.c_str(), port.c_str());
    fflush(stdout);
    for (;;) {
        if (!connection.connectTo(host, port)) {
            printf("cannot connect to %s:%s, retrying in 5 s...\n", host.c_str(), port.c_str());
            fflush(stdout);
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        printf("connected\n");
        fflush(stdout);
        connection.sendLine("hello " + name + " " + std::to_string(threads));
        bool alive = true;
        auto lastReport = std::chrono::steady_clock::now();
        while (alive) {
            int wanted;
            {
                std::lock_guard<std::mutex> lock(queueLock);
                wanted = threads + 2 - (int)queue.size() - busy;
            }
            if (wanted <= 0) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); continue; }
            if (!connection.sendLine("get " + std::to_string(wanted))) break;
            int received = 0;
            std::string line;
            for (;;) {
                if (!connection.readLine(line)) { alive = false; break; }
                if (line == "end") break;
                if (line.rfind("unit ", 0) != 0) continue;
                std::istringstream words(line.substr(5));
                Unit unit;
                std::string word;
                words >> unit.id;
                while (words >> word) unit.body += (unit.body.empty() ? "" : " ") + word;
                {
                    std::lock_guard<std::mutex> lock(queueLock);
                    queue.push_back(unit);
                }
                queueChanged.notify_one();
                received++;
            }
            if (alive && received == 0) std::this_thread::sleep_for(std::chrono::seconds(3));  // nothing to do yet
            if (std::chrono::steady_clock::now() - lastReport > std::chrono::seconds(60)) {
                double minutes = std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count() / 60;
                printf("%llu units solved in %.0f min\n", (unsigned long long)solvedCount.load(), minutes);
                fflush(stdout);
                lastReport = std::chrono::steady_clock::now();
            }
        }
        connection.close();
        {
            // units still queued here are re-queued by the coordinator when we disconnect
            std::lock_guard<std::mutex> lock(queueLock);
            queue.clear();
        }
        printf("connection lost, reconnecting...\n");
        fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
}

int cmdWorker(const Args& args) {
    if (!args.has(2)) { printf("usage: snek worker <coordinator-ip[:port]> [threads] [name]\n"); return 1; }
    int threads = args.arg(3, (int)std::max(1u, std::thread::hardware_concurrency()));
    char hostName[128] = "worker";
#ifdef _WIN32
    DWORD size = sizeof hostName;
    GetComputerNameA(hostName, &size);
#else
    gethostname(hostName, sizeof hostName);
#endif
    return runWorker(args.argv[2], threads, args.arg(4, hostName));
}
