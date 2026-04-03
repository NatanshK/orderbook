#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <string>
#include <chrono>

int main()
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in serv_addr;
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &serv_addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0)
    {
        std::cerr << "Connection Failed. Is the server running?\n";
        return -1;
    }

    const int NUM_ORDERS = 100'000;
    std::cout << "[TEST] Connected. Blasting " << NUM_ORDERS << " orders over TCP...\n";

    auto start = std::chrono::high_resolution_clock::now();

    std::string payload = "";
    // We batch orders into 8KB chunks to maximize TCP throughput
    for (int i = 1; i <= NUM_ORDERS; ++i)
    {
        payload += "ADD " + std::to_string(i) + " BUY 100 10\n";

        if (payload.size() > 8000)
        {
            send(sock, payload.c_str(), payload.size(), 0);
            payload.clear();
        }
    }

    if (!payload.empty())
    {
        send(sock, payload.c_str(), payload.size(), 0);
    }

    std::string shutdown = "SHUTDOWN\n";
    send(sock, shutdown.c_str(), shutdown.size(), 0);

    std::cout << "[TEST] Data sent. Waiting for server ACK...\n";
    char ack_buf[256];
    memset(ack_buf, 0, sizeof(ack_buf));

    recv(sock, ack_buf, sizeof(ack_buf) - 1, 0);

    auto end = std::chrono::high_resolution_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    std::cout << "[TEST] Server acknowledged! Total Round-Trip Time: " << duration_ms << " ms.\n";

    close(sock);
    return 0;
}