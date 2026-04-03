#pragma once
#include "OrderBook.hpp"
#include <string>
#include <unordered_map>
#include <vector>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/event.h>
#include <unistd.h>
#include <fcntl.h>

class TCPServer
{
public:
    // We pass the engine by reference so the server can push orders into it
    TCPServer(OrderBook &engine, uint16_t port);
    ~TCPServer();

    // Starts the kqueue event loop (this will block the main thread)
    void start();

private:
    OrderBook &engine_;
    int server_fd_;
    int kq_; // The kqueue file descriptor
    uint16_t port_;
    bool is_running_;

    // Maps a client's OS file descriptor to their incoming TCP text stream
    std::unordered_map<int, std::string> client_buffers_;

    void setNonBlocking(int fd);
    void handleNewConnection();
    void handleClientData(int client_fd);
    void disconnectClient(int client_fd);

    void parseAndRouteCommand(int client_fd, const std::string &command);

    std::vector<std::string> splitString(const std::string &str, char delimiter);
};