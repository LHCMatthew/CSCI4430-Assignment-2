#include <../common/LoadBalancerProtocol.h>
#include <spdlog/spdlog.h>
#include <string.h>
#include <stdlib.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <queue>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
struct ServerInfo {
    std::string ip;
    uint16_t port;
};

struct Node {
    enum Type { CLIENT, SWITCH, SERVER } type;
    std::string ip;
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<std::vector<std::pair<int, int>>> adj_list;
};

class LoadBalancer {
public:
    LoadBalancer(int p, bool r, bool g, const std::string& file)
        : port(p), rr(r), geo(g), servers_file(file) {}
    void run() {
        if (rr) {
            if (!parseRRFile()) {
                std::cerr << "Error parsing servers file\n";
                return;
            }
        }
        else if (geo) {
            if (!parseGeoFile()) {
                std::cerr << "Error parsing servers file\n";
                return;
            }
        }

        int server_id = 0;
        int opt = 1;
        struct sockaddr_in address;
        socklen_t addrlen = sizeof(address);
        
        if ((server_id = socket(AF_INET, SOCK_STREAM, 0)) < 0)
        {
            std::cerr << "Error: failed to create socket\n";
            return;
        }

        if (setsockopt(server_id, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
        {
            std::cerr << "Error: failed to set socket options\n";
            return;
        }

        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if (bind(server_id, (struct sockaddr*)&address, sizeof(address)) < 0)
        {
            std::cerr << "Error: bind failed\n";
            close(server_id);
            return;
        }

        if (listen(server_id, 5) < 0)
        {
            std::cerr << "Error: listen failed\n";
            close(server_id);
            return;
        }
        spdlog::info("Load balancer started on port {}", port);

        while (true) {
            struct sockaddr_in client_addr;
            int addrlen = sizeof(client_addr);
            int client_sock;
            if ((client_sock = accept(server_id, (struct sockaddr*)&client_addr, (socklen_t*)&addrlen)) < 0)
            {
                std::cerr << "Error: accept failed\n";
                continue;
            }
            handle_client(client_sock);
        }

        close(server_id);
    }
private:
    int port;
    bool rr;
    bool geo;
    std::string servers_file;
    std::vector<ServerInfo> servers;
    Graph graph;
    int current_rr_index = 0;

    bool parseRRFile() {
        std::ifstream file(servers_file);
        if (!file.is_open()) {
            std::cerr << "Error: could not open servers file\n";
            return false;
        }

        std::string line;
        int num_servers = 0;
        if (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string temp;
            iss >> temp >> num_servers;
        }

        while (std::getline(file, line)) {
            std::istringstream iss(line);
            ServerInfo server;
            iss >> server.ip >> server.port;
            servers.push_back(server);
        }
        file.close();
        return true;
    }
    bool parseGeoFile() {
        std::ifstream file(servers_file);
        if (!file.is_open()) {
            std::cerr << "Error: could not open servers file\n";
            return false;
        }

        std::string line;
        int num_nodes = 0;
        int num_links = 0;
        if (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string temp;
            iss >> temp >> num_nodes;
        }

        graph.nodes.resize(num_nodes);
        graph.adj_list.resize(num_nodes);

        for (int i = 0; i < num_nodes; ++i) {
            if (std::getline(file, line)) {
                std::istringstream iss(line);
                std::string type_str, ip_str;
                iss >> type_str >> ip_str;
                graph.nodes[i].type = (type_str == "CLIENT") ? Node::CLIENT :
                                      (type_str == "SWITCH") ? Node::SWITCH :
                                      Node::SERVER;
                graph.nodes[i].ip = ip_str;
            }
        }

        if (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string temp;
            iss >> temp >> num_links;
        }

        for (int i = 0; i < num_links; ++i) {
            if (std::getline(file, line)) {
                std::istringstream iss(line);
                int origin, dest, cost;
                iss >> origin >> dest >> cost;
                graph.adj_list[origin].push_back({dest, cost});
                graph.adj_list[dest].push_back({origin, cost}); // undirected graph
            }
        }

        file.close();
        return true;
    }
    int find_closest_server(int client_node) {
        std::priority_queue<std::pair<int, int>, std::vector<std::pair<int, int>>, std::greater<std::pair<int, int>>> pq;
        std::vector<int> dist(graph.nodes.size(), INT32_MAX);

        pq.push({0, client_node});
        dist[client_node] = 0;

        while (!pq.empty()) {
            auto [d, u] = pq.top();
            pq.pop();

            if (d > dist[u]) continue;

            for (auto [v, cost] : graph.adj_list[u]) {
                if (dist[v] > dist[u] + cost) {
                    dist[v] = dist[u] + cost;
                    pq.push({dist[v], v});
                }
            }
        }

        int closest_server = -1;
        int min_dist = INT32_MAX;
        for (int i = 0; i < graph.nodes.size(); i++) {
            if (graph.nodes[i].type == Node::SERVER && dist[i] < min_dist) {
                min_dist = dist[i];
                closest_server = i;
            }
        }

        return closest_server;
    }
    void handle_client(int client_sock) {
        LoadBalancerRequest request;
        ssize_t n = recv(client_sock, &request, sizeof(request), 0);

        if (n != sizeof(request)) {
            close(client_sock);
            return;
        }

        struct in_addr addr;
        addr.s_addr = request.client_addr;
        std::string client_ip = inet_ntoa(addr);
        uint16_t request_id_h = ntohs(request.request_id);
        spdlog::info("Received request for client {} with request ID {}", client_ip, request_id_h);
        
        LoadBalancerResponse response;
        response.request_id = request.request_id;

        bool can_send = false;

        if (rr) {
            ServerInfo server = servers[current_rr_index++ % servers.size()];
            inet_pton(AF_INET, server.ip.c_str(), &response.videoserver_addr);
            response.videoserver_port = htons(server.port);
            can_send = true;
        }
        else if (geo) {
            int client_node = -1;
            for (size_t i = 0; i < graph.nodes.size(); ++i) {
                if (graph.nodes[i].ip == client_ip) {
                    client_node = i;
                    break;
                }
            }
            if (client_node != -1) {
                int server_node = find_closest_server(client_node);
                if (server_node != -1) {
                    inet_pton(AF_INET, graph.nodes[server_node].ip.c_str(), &response.videoserver_addr);
                    response.videoserver_port = htons(8000);
                    can_send = true;
                }
            }
        }

        if (can_send) {
            send(client_sock, &response, sizeof(response), 0);

            struct in_addr vs_addr;
            vs_addr.s_addr = response.videoserver_addr;
            std::string vs_ip = inet_ntoa(vs_addr);
            uint16_t vs_port = ntohs(response.videoserver_port);
            spdlog::info("Responded to request ID {} with server {}:{}", request_id_h, vs_ip, vs_port);
        }
        else {
            spdlog::info("Failed to fulfill request ID {}", request_id_h);
        }

        close(client_sock);
    }
};

int main(int argc, const char** argv)
{
    int port = 0;
    bool rr = false;
    bool geo = false;
    std::string servers_file;
    if (argc > 6) {
        std::cerr << "Error: extra arguments\n";
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if(strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) {
                port = atoi(argv[i + 1]);
                if (port < 1024 || port > 65535) {
                    std::cerr << "Error: port must be in range [1024, 65535]\n";
                    return 1;
                }
                i++;
            } 
            else {
                std::cerr << "Error: missing port argument\n";
                return 1;
            }
        }
        else if (strcmp(argv[i], "-s") == 0 || strcmp(argv[i], "--servers") == 0) {
            if (i + 1 < argc) {
                servers_file = argv[i + 1];
                i++;
            } 
            else {
                std::cerr << "Error: missing server address argument\n";
                return 1;
            }
        }
        else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--rr") == 0) {
            rr = true;
        }
        else if (strcmp(argv[i], "-g") == 0 || strcmp(argv[i], "--geo") == 0) {
            geo = true;
        }
        else {
            std::cerr << "Error: extra arguments\n";
            return 1;
        }
    }
    if ((rr && geo) || (!rr && !geo)) {
        std::cerr << "Error: must choose either geo or rr load balancing strategy\n";
        return 1;
    }
    LoadBalancer lb(port, rr, geo, servers_file);
    lb.run();
    return 0;
}