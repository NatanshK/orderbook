#include "OrderBook.hpp"
#include "TCPServer.hpp"
#include <iostream>

int main()
{
    std::cout << "Starting high-frequency trading engine...\n";

    OrderBook engine;

    TCPServer server(engine, 8080);

    server.start();

    return 0;
}