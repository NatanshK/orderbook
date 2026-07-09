
// Order Book Stress Test  (protocol-correct, multi-phase, concurrent)

#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <chrono>
#include <random>
#include <vector>
#include <cstring>
#include <cstdint>
#include <thread>
#include <atomic>
#include <mutex>
#include <csignal>

#pragma pack(push, 1)
struct SnapshotHeader
{
    char message_type; // 'S'
    uint32_t num_asks;
    uint32_t num_bids;
};
struct LevelData
{
    uint64_t price;
    uint32_t volume;
};
#pragma pack(pop)

static int connectToServer(const char *ip = "127.0.0.1", uint16_t port = 8080)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return -1;

    sockaddr_in serv_addr{};
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);
    inet_pton(AF_INET, ip, &serv_addr.sin_addr);

    if (connect(sock, (sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        close(sock);
        return -1;
    }
    return sock;
}

// recv exactly n bytes (TCP can fragment); returns false on EOF/error
static bool recvAll(int sock, void *buf, size_t n)
{
    char *p = static_cast<char *>(buf);
    size_t got = 0;
    while (got < n)
    {
        ssize_t r = recv(sock, p + got, n - got, 0);
        if (r <= 0)
            return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

struct BookView
{
    std::vector<LevelData> asks; // high -> low, as the server sends them
    std::vector<LevelData> bids; // high -> low
    bool ok = false;
};

// Send VIEW and decode the binary snapshot. Uses its own connection so it can
// run concurrently with the load-generating sockets.
static BookView doView(int sock)
{
    BookView v;
    const char *cmd = "VIEW\n";
    if (send(sock, cmd, strlen(cmd), 0) < 0)
        return v;

    SnapshotHeader h{};
    if (!recvAll(sock, &h, sizeof(h)))
        return v;
    if (h.message_type != 'S')
        return v;

    v.asks.resize(h.num_asks);
    for (uint32_t i = 0; i < h.num_asks; ++i)
        if (!recvAll(sock, &v.asks[i], sizeof(LevelData)))
            return v;

    v.bids.resize(h.num_bids);
    for (uint32_t i = 0; i < h.num_bids; ++i)
        if (!recvAll(sock, &v.bids[i], sizeof(LevelData)))
            return v;

    v.ok = true;
    return v;
}

// A buffered sender: coalesce commands, flush past a threshold.
struct Sender
{
    int sock;
    std::string buf;
    void add(const std::string &cmd)
    {
        buf += cmd;
        if (buf.size() > 8000)
            flush();
    }
    void flush()
    {
        if (!buf.empty())
        {
            send(sock, buf.c_str(), buf.size(), 0);
            buf.clear();
        }
    }
};

static constexpr int NUM_ORDERS = 100'000;    // resting book depth (phase 1)
static constexpr int NUM_CLIENTS = 4;         // concurrent load generators (phase 4)
static constexpr int OPS_PER_CLIENT = 50'000; // mixed ops per client (phase 4)
static constexpr uint16_t PORT = 8080;

// Distinct id ranges so concurrent clients never collide.
static uint64_t clientIdBase(int client_idx) { return 10'000'000ULL * (client_idx + 1); }

int main()
{

    signal(SIGPIPE, SIG_IGN);

    std::cout << "[TEST] Order book stress test starting.\n";

    int main_sock = connectToServer("127.0.0.1", PORT);
    if (main_sock < 0)
    {
        std::cerr << "[TEST] Connection failed. Is the server running on port "
                  << PORT << "?\n";
        return -1;
    }
    std::cout << "[TEST] Connected.\n";

    std::mt19937 rng(42); // reproducible
    std::uniform_int_distribution<int> buy_price(90, 99);
    std::uniform_int_distribution<int> sell_price(101, 110);
    std::uniform_int_distribution<int> qty_dist(1, 20);

    auto t_start = std::chrono::high_resolution_clock::now();

    // PHASE 1: build a deep, non-crossing book
    //   Buys at 90-99, sells at 101-110. Spread never crosses, so nothing matches.
    {
        Sender s{main_sock, {}};
        std::vector<uint64_t> resting_ids;
        resting_ids.reserve(NUM_ORDERS);

        for (int i = 1; i <= NUM_ORDERS; ++i)
        {
            bool is_buy = (i % 2 == 1);
            int price = is_buy ? buy_price(rng) : sell_price(rng);
            int qty = qty_dist(rng);
            s.add("ADD " + std::to_string(i) + (is_buy ? " BUY " : " SELL ") +
                  std::to_string(price) + " " + std::to_string(qty) + "\n");
            resting_ids.push_back(i);
        }
        s.flush();
        std::cout << "[PHASE 1] Sent " << NUM_ORDERS << " resting orders.\n";

        // PHASE 2: O(1) cancels — cancel ~10% of resting orders (real verb: CAN)
        std::uniform_int_distribution<size_t> idx(0, resting_ids.size() - 1);
        int n_cancel = NUM_ORDERS / 10;
        for (int i = 0; i < n_cancel; ++i)
            s.add("CAN " + std::to_string(resting_ids[idx(rng)]) + "\n");
        s.flush();
        std::cout << "[PHASE 2] Sent " << n_cancel << " cancels.\n";

        // PHASE 3: MOD churn — move / resize some resting orders
        //   Mix of in-place (qty down) and cancel-replace (price change / qty up)

        int n_mod = NUM_ORDERS / 10;
        for (int i = 0; i < n_mod; ++i)
        {
            uint64_t id = resting_ids[idx(rng)];
            int new_price = (i % 2) ? buy_price(rng) : sell_price(rng);
            int new_qty = qty_dist(rng);
            s.add("MOD " + std::to_string(id) + " " + std::to_string(new_price) +
                  " " + std::to_string(new_qty) + "\n");
        }
        s.flush();
        std::cout << "[PHASE 3] Sent " << n_mod << " modifies.\n";
    }

    // PHASE 4: concurrent load + reader contention
    std::atomic<bool> poller_run{true};
    std::atomic<uint64_t> view_count{0};
    std::atomic<bool> view_consistent{true};

    std::thread poller([&]()
                       {
        int psock = connectToServer("127.0.0.1", PORT);
        if (psock < 0)
            return;
        while (poller_run.load(std::memory_order_relaxed))
        {
            BookView v = doView(psock);
            if (v.ok)
            {
                view_count.fetch_add(1, std::memory_order_relaxed);
                if (v.asks.size() > 5 || v.bids.size() > 5)
                    view_consistent.store(false, std::memory_order_relaxed);
            }
        }
        close(psock); });

    std::vector<std::thread> clients;
    std::atomic<uint64_t> client_ops{0};

    auto t_phase4 = std::chrono::high_resolution_clock::now();
    for (int c = 0; c < NUM_CLIENTS; ++c)
    {
        clients.emplace_back([&, c]()
                             {
            int csock = connectToServer("127.0.0.1", PORT);
            if (csock < 0)
                return;

            std::mt19937 lrng(1000 + c);
            std::uniform_int_distribution<int> price_d(85, 115);
            std::uniform_int_distribution<int> qty_d(1, 20);
            std::uniform_int_distribution<int> op_d(0, 99);
            std::uniform_int_distribution<int> side_d(0, 1);

            Sender    s{csock, {}};
            uint64_t  base = clientIdBase(c);
            uint64_t  next_id = base;
            std::vector<uint64_t> my_ids;

            for (int i = 0; i < OPS_PER_CLIENT; ++i)
            {
                int roll = op_d(lrng);
                if (roll < 60 || my_ids.empty())
                {
                    uint64_t id   = next_id++;
                    bool     buy  = side_d(lrng);
                    int      px   = price_d(lrng);
                    int      qty  = qty_d(lrng);
                    std::string t = (roll < 45) ? "" : (roll < 53) ? " IOC" : " MARKET";
                    s.add("ADD " + std::to_string(id) + (buy ? " BUY " : " SELL ") +
                          std::to_string(px) + " " + std::to_string(qty) + t + "\n");
                    if (t.empty())
                        my_ids.push_back(id);
                }
                else if (roll < 80)
                {
                    uint64_t id = my_ids[lrng() % my_ids.size()];
                    s.add("CAN " + std::to_string(id) + "\n");
                }
                else
                {
                    uint64_t id = my_ids[lrng() % my_ids.size()];
                    s.add("MOD " + std::to_string(id) + " " +
                          std::to_string(price_d(lrng)) + " " +
                          std::to_string(qty_d(lrng)) + "\n");
                }
                client_ops.fetch_add(1, std::memory_order_relaxed);
            }
            s.flush();
            close(csock); });
    }
    for (auto &th : clients)
        th.join();
    auto t_phase4_end = std::chrono::high_resolution_clock::now();

    poller_run.store(false);
    poller.join();

    double phase4_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t_phase4_end - t_phase4).count();
    uint64_t total_client_ops = client_ops.load();
    std::cout << "[PHASE 4] " << NUM_CLIENTS << " clients sent " << total_client_ops
              << " mixed ops in " << phase4_ms << " ms  ("
              << (phase4_ms > 0 ? (total_client_ops / phase4_ms * 1000.0) : 0.0)
              << " ops/s client-observed).\n";
    std::cout << "[PHASE 4] Poller completed " << view_count.load()
              << " VIEW snapshots concurrently. Depth-cap consistent: "
              << (view_consistent.load() ? "YES" : "NO") << "\n";

    // PHASE 5: aggressive sweep
    {
        // settle: wait until the visible ask side stops changing
        int settle_sock = connectToServer("127.0.0.1", PORT);
        BookView pv = doView(settle_sock);
        int stable = 0;
        auto sdl = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < sdl)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            BookView cv = doView(settle_sock);
            bool same = cv.ok && pv.ok && cv.asks.size() == pv.asks.size();
            if (same)
            {
                if (++stable >= 3)
                    break;
            }
            else
                stable = 0;
            pv = cv;
        }
        close(settle_sock);

        Sender s{main_sock, {}};
        const int SWEEP_ORDERS = 200; // moderate aggressive orders
        const int SWEEP_QTY = 8000;   // each sweeps many levels; 200*8000 = 1.6M
        uint64_t sweep_id = 900000;
        for (int i = 0; i < SWEEP_ORDERS; ++i)
            s.add("ADD " + std::to_string(sweep_id++) +
                  " BUY 100000 " + std::to_string(SWEEP_QTY) + "\n");
        s.flush();
        std::cout << "[PHASE 5] Book settled, then sent " << SWEEP_ORDERS
                  << " aggressive crossing BUYs (sweeps all asks in chunks).\n";
    }

    // PHASE 6: drain + verify

    {
        int vsock = connectToServer("127.0.0.1", PORT);
        auto describe = [](const BookView &v)
        {
            std::string s = "asks=" + std::to_string(v.asks.size()) +
                            " bids=" + std::to_string(v.bids.size());
            if (!v.bids.empty())
                s += " bestBid=" + std::to_string(v.bids.front().price);
            if (!v.asks.empty())
                s += " bestAsk=" + std::to_string(v.asks.back().price);
            return s;
        };

        BookView prev = doView(vsock);
        int stable = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            BookView cur = doView(vsock);
            bool same = cur.ok && prev.ok &&
                        cur.asks.size() == prev.asks.size() &&
                        cur.bids.size() == prev.bids.size();
            if (same)
            {
                if (++stable >= 3)
                    break;
            }
            else
                stable = 0;
            prev = cur;
        }

        BookView finalv = doView(vsock);
        std::cout << "[PHASE 6] Final book: " << describe(finalv) << "\n";

        // After a settled 1.6M-unit sweep against a book that never exceeds
        // ~1.05M resting units, the ask side should be fully consumed and the
        // sweep remainder rests as a large bid at the sweep price (100000).
        uint64_t total_ask_vol = 0;
        for (const auto &lvl : finalv.asks)
            total_ask_vol += lvl.volume;

        bool asks_cleared = finalv.asks.empty();
        bool remainder_rested = !finalv.bids.empty() &&
                                finalv.bids.front().price == 100000;
        std::cout << "[PHASE 6] Ask side fully swept: "
                  << (asks_cleared ? "YES" : "NO")
                  << " (residual ask volume=" << total_ask_vol << ")\n";
        std::cout << "[PHASE 6] Sweep remainder rested as bid @100000: "
                  << (remainder_rested ? "YES" : "NO") << "\n";

        close(vsock);
    }

    // Shutdown: server prints its INTERNAL latency report.
    std::string shutdown = "SHUTDOWN\n";
    send(main_sock, shutdown.c_str(), shutdown.size(), 0);

    char ack[256];
    memset(ack, 0, sizeof(ack));
    recv(main_sock, ack, sizeof(ack) - 1, 0);

    auto t_end = std::chrono::high_resolution_clock::now();
    auto total_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();

    std::cout << "[TEST] Server acknowledged shutdown.\n";
    std::cout << "[TEST] Total wall time (all phases): " << total_ms << " ms.\n";
    std::cout << "[TEST] See server console for the engine's internal latency report.\n";

    close(main_sock);
    return 0;
}