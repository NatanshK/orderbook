#include "TCPServer.hpp"
#include <iostream>
#include <sstream>
#include <string.h>

#pragma pack(push, 1)
struct SnapshotHeader
{
    char message_type; // 1 byte ('S' for Snapshot)
    uint32_t num_asks; // 4 bytes
    uint32_t num_bids; // 4 bytes
}; // Total: 9 bytes

struct LevelData
{
    uint64_t price;  // 8 bytes
    uint32_t volume; // 4 bytes
}; // Total: 12 bytes
#pragma pack(pop)

// CONSTRUCTOR
TCPServer::TCPServer(OrderBook &engine, uint16_t port)
    : engine_(engine), port_(port), is_running_(true)
{

    // Raw IPv4 TCP socket file descriptor
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0)
    {
        std::cerr << "[ERROR] Failed to create socket.\n";
        exit(1);
    }

    // Allowing the OS to reuse the port immediately after crash/restart
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Binding the socket to our specific port on localhost
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port_);

    if (bind(server_fd_, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        std::cerr << "[ERROR] Failed to bind to port " << port_ << ".\n";
        exit(1);
    }

    listen(server_fd_, SOMAXCONN);

    // The listening socket is non-blocking
    setNonBlocking(server_fd_);

    kq_ = kqueue();
    if (kq_ < 0)
    {
        std::cerr << "[ERROR] Failed to create kqueue.\n";
        exit(1);
    }

    struct kevent evSet;
    EV_SET(&evSet, server_fd_, EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(kq_, &evSet, 1, NULL, 0, NULL);

    std::cout << "[NETWORK] TCP Server listening on port " << port_ << " via kqueue.\n";
}

TCPServer::~TCPServer()
{
    is_running_ = false;
    close(server_fd_);
    close(kq_);
}

void TCPServer::setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void TCPServer::start()
{
    struct kevent evList[32]; // Buffer to hold OS event notifications

    while (is_running_)
    {
        int num_events = kevent(kq_, NULL, 0, evList, 32, NULL);

        for (int i = 0; i < num_events; i++)
        {
            int current_fd = evList[i].ident;

            // Scenario A: A client disconnected
            if (evList[i].flags & EV_EOF)
            {
                disconnectClient(current_fd);
            }
            // Scenario B: A NEW client is trying to connect to the server
            else if (current_fd == server_fd_)
            {
                handleNewConnection();
            }
            // Scenario C: An EXISTING client sent us text data
            else if (evList[i].filter == EVFILT_READ)
            {
                handleClientData(current_fd);
            }
        }
    }
}

void TCPServer::handleNewConnection()
{
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(server_fd_, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0)
        return;

    setNonBlocking(client_fd);

    struct kevent evSet;
    EV_SET(&evSet, client_fd, EVFILT_READ, EV_ADD, 0, 0, NULL);
    kevent(kq_, &evSet, 1, NULL, 0, NULL);

    std::cout << "[NETWORK] New client connected. FD: " << client_fd << "\n";
}

void TCPServer::disconnectClient(int client_fd)
{
    std::cout << "[NETWORK] Client disconnected. FD: " << client_fd << "\n";
    close(client_fd);
    client_buffers_.erase(client_fd);
}

void TCPServer::handleClientData(int client_fd)
{
    char buffer[4096];
    memset(buffer, 0, sizeof(buffer));

    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);

    if (bytes_read > 0)
    {
        client_buffers_[client_fd].append(buffer, bytes_read);

        // Checking if we have a full message (ended by a newline)
        size_t pos;
        while ((pos = client_buffers_[client_fd].find('\n')) != std::string::npos)
        {
            std::string command = client_buffers_[client_fd].substr(0, pos);
            client_buffers_[client_fd].erase(0, pos + 1);

            if (!command.empty() && command.back() == '\r')
            {
                command.pop_back();
            }

            // Skipping random empty lines caused by packet fragmentation
            if (command.empty())
                continue;

            parseAndRouteCommand(client_fd, command);
        }
    }
    else if (bytes_read == 0)
    {
        disconnectClient(client_fd);
    }
}

void TCPServer::parseAndRouteCommand(int client_fd, const std::string &command)
{
    std::vector<std::string> tokens;
    std::stringstream ss(command);
    std::string token;
    while (ss >> token)
    {
        tokens.push_back(token);
    }

    if (tokens.empty())
        return;

    try
    {
        if (tokens[0] == "ADD" && tokens.size() >= 5)
        {
            Order o{};
            o.order_id = std::stoull(tokens[1]);
            o.side = (tokens[2] == "BUY") ? Side::BUY : Side::SELL;
            o.price = std::stoull(tokens[3]);
            o.quantity = std::stoul(tokens[4]);
            o.type = Type::LIMIT; // default

            if (tokens.size() == 6)
            {
                if (tokens[5] == "MARKET")
                    o.type = Type::MARKET;
                else if (tokens[5] == "IOC")
                    o.type = Type::IOC;
            }

            engine_.addOrder(o);
        }
        else if (tokens[0] == "CAN" && tokens.size() == 2)
        {
            uint64_t order_id = std::stoull(tokens[1]);
            engine_.submitCancel(order_id);
            std::string ack = "ACK CAN\n";
            send(client_fd, ack.c_str(), ack.size(), 0);
        }
        else if (tokens[0] == "MOD" && tokens.size() == 4)
        {
            uint64_t order_id = std::stoull(tokens[1]);
            uint64_t new_price = std::stoull(tokens[2]);
            uint32_t new_qty = std::stoul(tokens[3]);
            engine_.submitModify(order_id, new_price, new_qty);
            std::string ack = "ACK MOD\n";
            send(client_fd, ack.c_str(), ack.size(), 0);
        }
        else if (tokens[0] == "VIEW")
        {
            OrderBookSnapshot snap = engine_.getSnapshot(5);

            SnapshotHeader header;
            header.message_type = 'S';
            header.num_asks = snap.asks.size();
            header.num_bids = snap.bids.size();

            send(client_fd, &header, sizeof(header), 0);

            for (auto it = snap.asks.rbegin(); it != snap.asks.rend(); ++it)
            {
                LevelData ld;
                ld.price = it->price;
                ld.volume = it->total_quantity;
                send(client_fd, &ld, sizeof(ld), 0);
            }

            for (const auto &level : snap.bids)
            {
                LevelData ld;
                ld.price = level.price;
                ld.volume = level.total_quantity;
                send(client_fd, &ld, sizeof(ld), 0);
            }
        }

        else if (tokens[0] == "SHUTDOWN")
        {
            std::cout << "[SYSTEM] Shutdown command received. Generating report...\n";
            engine_.printLatencyStats();
            is_running_ = false;

            std::string ack = "ACK SHUTDOWN\n";
            send(client_fd, ack.c_str(), ack.size(), 0);
        }
        else
        {
            std::string err = "ERR INVALID_COMMAND\n";
            send(client_fd, err.c_str(), err.size(), 0);
        }
    }
    catch (const std::exception &e)
    {
        std::cerr << "[WARNING] Dropped malformed packet: '" << command << "' | Error: " << e.what() << "\n";
    }
}