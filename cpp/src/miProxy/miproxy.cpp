#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h> 
#include <arpa/inet.h> 
#include <sys/time.h>
#include <unistd.h>

#define MAXCLIENTS 30
#define BUFFER_SIZE 8192

struct Config
{
    bool balance = false;
    int listen_port = 0;
    std::string host_name = "";
    int port = 0;
    double alpha = 0.0;
};

struct HttpRequest
{
    std::string method;
    std::string url;
    std::string version;
    std::string headers;
};

Config handle_args(int argc, char** argv)
{
    Config config;
    for (int i = 1; i < argc; ++i)
    {
        std::string ar = argv[i];
        if (ar == "-b")
        {
            config.balance = true;
        }
        else if (ar == "-l")
        {
            if (i + 1 < argc)
            {
                i++;
                config.listen_port = std::stoi(argv[i]);
            }
            else
            {
                std::cout << "Error: missing log file argument\n";
                exit(1);
            }
        }
        else if (ar == "-h")
        {
            if (i + 1 < argc)
            {
                i++;
                config.host_name = argv[i];
            }
            else
            {
                std::cout << "Error: missing host name argument\n";
                exit(1);
            }
        }
        else if (ar == "-p")
        {
            if (i + 1 < argc)
            {
                i++;
                config.port = std::stoi(argv[i]);
                if (config.port < 1024 || config.port > 65535)
                {
                    std::cout << "Error: port number must be in the range of [1024, 65535]\n";
                    exit(1);
                }
            }
            else
            {
                std::cout << "Error: missing port number argument\n";
                exit(1);
            }
        }
        else if (ar == "-a")
        {
            if (i + 1 < argc)
            {
                i++;
                config.alpha = std::stod(argv[i]);
                if (config.alpha < 0.0 || config.alpha > 1.0)
                {
                    std::cout << "Error: alpha must be in the range of [0.0, 1.0]\n";
                    exit(1);
                }
            }
            else
            {
                std::cout << "Error: missing alpha argument\n";
                exit(1);
            }
        }
        else
        {
            std::cout << "Error: missing or extra arguments\n";
            exit(1);
        }
    }

    if (config.listen_port == 0 || config.host_name == "" || config.port == 0)
    {
        std::cout << "Error: missing or extra arguments\n";
        exit(1);
    }

    return config;
}

void parse_http_request(const std::string& request_str, HttpRequest& request)
{
    size_t method_end = request_str.find(' ');
    if (method_end != std::string::npos)
    {
        request.method = request_str.substr(0, method_end);
        size_t url_end = request_str.find(' ', method_end + 1);
        if (url_end != std::string::npos)
        {
            request.url = request_str.substr(method_end + 1, url_end - method_end - 1);
            size_t version_end = request_str.find("\r\n", url_end + 1);
            if (version_end != std::string::npos)
            {
                request.version = request_str.substr(url_end + 1, version_end - url_end - 1);
            }
        }
    }
}

int main(int argc, char **argv)
{
    if (argc < 9)
    {
        std::cout << "Error: missing or extra arguments\n";
        return 1;
    }
    Config config = handle_args(argc, argv);

    int server_id = 0;
    int opt = 1;
    struct sockaddr_in address;
    socklen_t addrlen = sizeof(address);
    
    if ((server_id = socket(AF_INET, SOCK_STREAM, 0)) < 0)
    {
        std::cout << "Error: failed to create socket\n";
        return 1;
    }

    if (setsockopt(server_id, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        std::cout << "Error: failed to set socket options\n";
        return 1;
    }

    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(config.port);

    if (bind(server_id, (struct sockaddr*)&address, sizeof(address)) < 0)
    {
        std::cout << "Error: bind failed\n";
        return 1;
    }

    if (listen(server_id, 5) < 0)
    {
        std::cout << "Error: listen failed\n";
        return 1;
    }
    std::cout << "Listening on port " << config.port << "\n";

    int activity, valread;
    int client_sockets[MAXCLIENTS] = {0};

    int client_sock;
    fd_set readfds;
    while (1)
    {
        // // clear the socket set
        // FD_ZERO(&readfds);

        // // add master socket to set
        // FD_SET(server_id, &readfds);
        // for (int i = 0; i < MAXCLIENTS; i++) {
        //     client_sock = client_sockets[i];
        //     if (client_sock != 0) {
        //         FD_SET(client_sock, &readfds);
        //     }
        // }

        // // wait for an activity on one of the sockets , timeout is NULL ,
        // // so wait indefinitely
        // activity = select(FD_SETSIZE, &readfds, NULL, NULL, NULL);
        // if (activity < 0)
        // {
        //     std::cout << "Error: select error\n";
        //     return 1;
        // }

        // if (FD_ISSET(server_id, &readfds))
        // {
        //     int addrlen = sizeof(address);
        //     if ((client_sock = accept(server_id, (struct sockaddr*)&address, (socklen_t*)&addrlen)) < 0)
        //     {
        //         std::cout << "Error: accept failed\n";
        //         return 1;
        //     }
        //     std::cout << "New connection, socket fd is " << client_sock << "\n";

        //     // add new socket to array of sockets
        //     for (int i = 0; i < MAXCLIENTS; i++)
        //     {
        //         if (client_sockets[i] == 0)
        //         {
        //             client_sockets[i] = client_sock;
        //             break;
        //         }
        //     }
        // }

        // // else it's some IO operation on a client socket
        // for (int i = 0; i < MAXCLIENTS; i++) 
        // {
        //     client_sock = client_sockets[i];
        //     // Note: sd == 0 is our default here by fd 0 is actually stdin
        //     if (client_sock != 0 && FD_ISSET(client_sock, &readfds)) 
        //     {

        //     }
        // }
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_conn = accept(server_id, (struct sockaddr*)&client_addr, &addr_len);
        if (client_conn < 0) continue;
        std::cout << "New connection, socket fd is " << client_conn << "\n";

        // testing
        char buffer[BUFFER_SIZE];
        int bytes = recv(client_sock, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0)
        {
            std::cout << "Error: failed to receive data from client\n";
            continue;
        }
        buffer[bytes] = '\0';
        std::cout << "Received request:\n" << buffer << "\n";

#if 0
        // connect to the video server
        int server_conn = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(config.port);
        struct hostent* server = gethostbyname(config.host_name.c_str());
        memcpy(&(server_addr.sin_addr), server->h_addr, server->h_length);
        if (connect(server_conn, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
        {
            std::cout << "Error: connection to video server failed\n";
            close(client_conn);
            continue;
        }
        std::cout << "Connected to video server " << config.host_name << " on port " << config.port << "\n";
#endif

    }

    return 0;
}
            