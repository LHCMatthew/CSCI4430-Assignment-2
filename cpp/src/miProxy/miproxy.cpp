#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h> 
#include <arpa/inet.h> 
#include <sys/time.h>
#include <unistd.h>
#include <vector>
#include <cstring>
#include "pugixml.hpp"
#include "../common/LoadBalancerProtocol.h"
#include <random>
#include <spdlog/spdlog.h>
#include <map>

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

enum ProcessState
{
    HANDLE_CLIENT_REQUEST = 0,
    HANDLE_SERVER_RESPONSE = 1,
};

struct ClientInfo
{
    int client_fd;
    int server_fd;
    std::string client_ip;
    int client_port;
    std::string server_ip;
    int server_port;
    std::string uuid;
    double avg_throughput = 0.0;
    std::string current_video_path;
    ProcessState current_process_state = HANDLE_CLIENT_REQUEST;
};

std::map<std::string, std::vector<int>> video_cache; // video path -> available bitrates

class HttpMessage
{
public:
    void parseHttpRequest(const std::string& request_str)
    {
        size_t method_end = request_str.find(' ');
        if (method_end != std::string::npos)
        {
            method_ = request_str.substr(0, method_end);
            size_t url_end = request_str.find(' ', method_end + 1);
            if (url_end != std::string::npos)
            {
                url_ = request_str.substr(method_end + 1, url_end - method_end - 1);
                size_t version_end = request_str.find("\r\n", url_end + 1);
                if (version_end != std::string::npos)
                {
                    version_ = request_str.substr(url_end + 1, version_end - url_end - 1);
                    size_t headers_end = request_str.find("\r\n\r\n", version_end + 2);
                    if (headers_end != std::string::npos)
                    {
                        headers_ = request_str.substr(version_end + 2, headers_end - version_end - 2);
                    }
                }
            }
        }
    }

    void parseHttpResponse(const std::string& response_str)
    {
        size_t version_end = response_str.find(' ');
        if (version_end != std::string::npos)
        {
            version_ = response_str.substr(0, version_end);
            size_t status_code_end = response_str.find(' ', version_end + 1);
            if (status_code_end != std::string::npos)
            {
                status_code_ = std::stoi(response_str.substr(version_end + 1, status_code_end - version_end - 1));
                size_t status_message_end = response_str.find("\r\n", status_code_end + 1);
                if (status_message_end != std::string::npos)
                {
                    status_message_ = response_str.substr(status_code_end + 1, status_message_end - status_code_end - 1);
                    size_t headers_end = response_str.find("\r\n\r\n", status_message_end + 2);
                    if (headers_end != std::string::npos)
                    {
                        headers_ = response_str.substr(status_message_end + 2, headers_end - status_message_end - 2);
                    }
                }
            }
        }
    }

    std::string getHeaderValue(const std::string& header_name) 
    {
        size_t pos = headers_.find(header_name + ": ");
        if (pos != std::string::npos) 
        {
            size_t value_start = pos + header_name.length() + 2;
            size_t value_end = headers_.find("\r\n", value_start);
            if (value_end != std::string::npos) 
            {
                return headers_.substr(value_start, value_end - value_start);
            }
        }
        return "";
    }

    std::string getMethod() const { return method_; }
    std::string getUrl() const { return url_; }
    std::string getVersion() const { return version_; }
    std::string getHeaders() const { return headers_; }

private:
    std::string method_;
    std::string url_;

    // http response components
    int status_code_;
    std::string status_message_;

    std::string version_;
    std::string headers_;
};

std::string readHttpRequest(int sockfd) {
    std::string result;
    char buffer[1];
    std::string headers;
    
    // Read until we find \r\n\r\n
    while (true) {
        int n = recv(sockfd, buffer, 1, 0);
        // int n = recv(sockfd, buffer, 1, MSG_DONTWAIT);

        if (n <= 0) return "";
        
        headers += buffer[0];
        if (headers.size() >= 4 && 
            headers.substr(headers.size() - 4) == "\r\n\r\n") {
            break;
        }
    }
    
    result = headers;
    HttpMessage request;
    request.parseHttpRequest(headers);
    std::string content_length_str = request.getHeaderValue("content-length");
    int content_length = 0;
    if (!content_length_str.empty()) {
        content_length = std::stoi(content_length_str);
    }
    
    if (content_length > 0) {
        std::vector<char> body(content_length);
        int total_read = 0;
        while (total_read < content_length) {
            int n = recv(sockfd, body.data() + total_read, content_length - total_read, 0);
            if (n <= 0) break;
            total_read += n;
        }
        result.append(body.data(), total_read);
    }
    
    return result;
}

std::string readHttpResponse(int sockfd) {
    std::string result;
    char buffer[1];
    std::string headers;
    
    // Read until we find \r\n\r\n
    while (true) {
        int n = recv(sockfd, buffer, 1, 0);
        if (n <= 0) return "";
        
        headers += buffer[0];
        if (headers.size() >= 4 && 
            headers.substr(headers.size() - 4) == "\r\n\r\n") {
            break;
        }
    }
    
    result = headers;
    HttpMessage response;
    response.parseHttpResponse(headers);
    std::string content_length_str = response.getHeaderValue("content-length");
    int content_length = 0;
    if (!content_length_str.empty()) {
        content_length = std::stoi(content_length_str);
    }
    
    if (content_length > 0) {
        std::vector<char> body(content_length);
        int total_read = 0;
        while (total_read < content_length) {
            int n = recv(sockfd, body.data() + total_read, content_length - total_read, 0);
            if (n <= 0) break;
            total_read += n;
        }
        result.append(body.data(), total_read);
    }

    return result;
}

std::pair<std::string, int> queryLoadBalancer(const std::string& lb_ip, int lb_port, const std::string& client_ip) {
    // Create socket to connect to load balancer
    int lb_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (lb_sock < 0) {
        spdlog::error("Failed to create socket for load balancer");
        return {"", 0};
    }

    // Connect to load balancer
    struct sockaddr_in lb_addr;
    lb_addr.sin_family = AF_INET;
    lb_addr.sin_port = htons(lb_port);
    struct hostent* server = gethostbyname(lb_ip.c_str());
    if (!server) {
        spdlog::error("Failed to resolve load balancer hostname");
        close(lb_sock);
        return {"", 0};
    }
    memcpy(&(lb_addr.sin_addr), server->h_addr, server->h_length);

    if (connect(lb_sock, (struct sockaddr*)&lb_addr, sizeof(lb_addr)) < 0) {
        spdlog::error("Failed to connect to load balancer");
        close(lb_sock);
        return {"", 0};
    }

    // Prepare request
    LoadBalancerRequest request;
    inet_pton(AF_INET, client_ip.c_str(), &request.client_addr);
    
    // Generate random request ID
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint16_t> dis(0, 65535);
    request.request_id = htons(dis(gen));

    // Send request
    if (send(lb_sock, &request, sizeof(request), 0) < 0) {
        spdlog::error("Failed to send request to load balancer");
        close(lb_sock);
        return {"", 0};
    }

    // Receive response
    LoadBalancerResponse response;
    int bytes_received = recv(lb_sock, &response, sizeof(response), 0);
    close(lb_sock);

    if (bytes_received != sizeof(response)) {
        spdlog::error("Failed to receive complete response from load balancer");
        return {"", 0};
    }

    // Convert response to host order
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &response.videoserver_addr, ip_str, INET_ADDRSTRLEN);
    int port = ntohs(response.videoserver_port);

    return {std::string(ip_str), port};
}

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
    address.sin_port = htons(config.listen_port);

    if (bind(server_id, (struct sockaddr*)&address, sizeof(address)) < 0)
    {
        std::cout << "Error: bind failed\n";
        return 1;
    }

    if (listen(server_id, 10) < 0)
    {
        std::cout << "Error: listen failed\n";
        return 1;
    }
    spdlog::info("miProxy started");

    int activity, valread;
    int client_sockets[MAXCLIENTS] = {0};
    ClientInfo clients_info[MAXCLIENTS];

    int client_sock;
    fd_set readfds;

    while (1)
    {
        // clear the socket set
        FD_ZERO(&readfds);

        // add master socket to set
        FD_SET(server_id, &readfds);
        for (int i = 0; i < MAXCLIENTS; i++) {
            client_sock = client_sockets[i];
            if (client_sock != 0) {
                FD_SET(client_sock, &readfds);
            }
        }

        // wait for an activity on one of the sockets , timeout is NULL ,
        // so wait indefinitely
        activity = select(FD_SETSIZE, &readfds, NULL, NULL, NULL);
        if (activity < 0)
        {
            std::cout << "Error: select error\n";
            return 1;
        }

        if (FD_ISSET(server_id, &readfds))
        {
            struct sockaddr_in client_addr;
            int addrlen = sizeof(client_addr);
            if ((client_sock = accept(server_id, (struct sockaddr*)&client_addr, (socklen_t*)&addrlen)) < 0)
            {
                std::cout << "Error: accept failed\n";
                return 1;
            }

            int video_server_port = config.port;
            std::string video_server_ip = config.host_name;
            if (config.balance)
            {
                std::string client_ip = inet_ntoa(client_addr.sin_addr);
                auto result = queryLoadBalancer(config.host_name, config.port, client_ip);
                
                if (result.first.empty() || result.second == 0) {
                    spdlog::error("Failed to get video server from load balancer for client {}", client_ip);
                    close(client_sock);
                    continue;
                }
                
                video_server_ip = result.first;
                video_server_port = result.second;
            }

            int server_conn = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in server_addr;
            server_addr.sin_family = AF_INET;
            server_addr.sin_port = htons(video_server_port);
            struct hostent* server = gethostbyname(video_server_ip.c_str());
            memcpy(&(server_addr.sin_addr), server->h_addr, server->h_length);
            if (connect(server_conn, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0)
            {
                std::cout << "Error: connection to video server failed\n";
                close(client_sock);
                continue;
            }
            spdlog::info("New client socket connected with {}:{} on sockfd {}", config.host_name, config.port, client_sock); 

            ClientInfo info;
            info.client_fd = client_sock;
            info.client_ip = inet_ntoa(client_addr.sin_addr);
            info.client_port = ntohs(client_addr.sin_port);
            info.server_ip = config.host_name;
            info.server_port = config.port;
            info.server_fd = server_conn;

            // add new socket to array of sockets
            for (int i = 0; i < MAXCLIENTS; i++)
            {
                if (client_sockets[i] == 0)
                {
                    client_sockets[i] = client_sock;
                    clients_info[i] = info;
                    break;
                }
            }
        }

        // else it's some IO operation on a client socket
        for (int i = 0; i < MAXCLIENTS; i++) 
        {
            client_sock = client_sockets[i];
            int server_conn = clients_info[i].server_fd;
            // Note: sd == 0 is our default here by fd 0 is actually stdin
            if (client_sock != 0 && FD_ISSET(client_sock, &readfds) && server_conn != 0) 
            {
                // std::cout << "Http resquest from client:\n";
                std::string client_request = readHttpRequest(client_sock);
                // std::cout << client_request << "\n";
                if (client_request.empty()) 
                {
                    spdlog::info("Client socket sockfd {} disconnected", client_sock);
                    close(client_sock);
                    close(server_conn);
                    client_sockets[i] = 0;
                    continue;
                }

                HttpMessage request;
                request.parseHttpRequest(client_request);
                if (request.getMethod() == "GET")
                {
                    std::string url = request.getUrl();

                    // Handle manifest request
                    if (url.find(".mpd") != std::string::npos)
                    {
                        std::string original_url = url;
                        url = url.substr(0, url.find(".mpd")) + "-no-list.mpd";
                        std::cout << "Manifest requested, modified url: " << url << "\n";
                        client_request.replace(client_request.find(request.getUrl()), request.getUrl().length(), url);
                        send(server_conn, client_request.c_str(), client_request.size(), 0);

                        std::string tmp = "/videos/";
                        size_t video_path_end = url.find("/", tmp.length()+1);
                        std::string video_path = url.substr(tmp.length(), video_path_end - tmp.length());

                        // get the bit rates from the original manifest if not in cache
                        if (video_cache.find(video_path) == video_cache.end()) 
                        {
                            pugi::xml_document doc;
                            pugi::xml_parse_result result = doc.load_file(("../../../videoserver/static" + original_url).c_str());

                            if (!result) {
                                std::cout << "XML [" << original_url << "] parsed with errors, attr value: [" << doc.child("node").attribute("attr").value() << "]\n";
                                std::cout << "Error description: " << result.description() << "\n";
                                std::cout << "Error offset: " << result.offset << " (error at [..." << (original_url.c_str() + result.offset) << "]\n\n";
                            }

                            std::vector<int> bitrates;
                            for (pugi::xml_node adaptationSet : doc.child("MPD").child("Period").children("AdaptationSet")) 
                            {
                                for (pugi::xml_node representation : adaptationSet.children("Representation")) 
                                {
                                    int bitrate = representation.attribute("bandwidth").as_int();
                                    bitrates.push_back(bitrate);
                                }
                            }
                            video_cache[video_path] = bitrates;
                        }
                        spdlog::info("Manifest requested by {} forwarded to {}:{} for {}", clients_info[i].uuid, config.host_name, config.port, url);
                    }
                    else if (url.find("/video/") != std::string::npos && url.find(".m4s") != std::string::npos) 
                    {
                        std::string tmp = "/videos/";
                        size_t video_path_end = url.find("/", tmp.length()+1);
                        std::string video_path = url.substr(tmp.length(), video_path_end - tmp.length());
                        clients_info[i].current_video_path = video_path;

                        size_t bitrate_start = url.find("vid-", 0) + 4;
                        size_t bitrate_end = url.find("-seg", 0)-1;

                        std::string bitrate_str = url.substr(bitrate_start, bitrate_end - bitrate_start + 1);

                        if (!bitrate_str.empty()) 
                        {
                            // std::cout << "i: " << i << " Client requested video path: " << video_path << ", bitrate: " << bitrate_str << " url: " << url << "\n";
                            int client_bitrate = std::stoi(bitrate_str);
                            std::vector<int> available_bitrates = video_cache[video_path];
                            int selected_bitrate = available_bitrates[0];
                            int max_bitrate = INT_MIN;
                            for (int br : available_bitrates) 
                            {
                                if (br <= clients_info[i].avg_throughput / 1.5) // convert Kbps to bps
                                {
                                    if (br > max_bitrate)
                                    {
                                        max_bitrate = br;
                                        selected_bitrate = br;
                                    }
                                }
                            }

                            // modify the url to request the selected bitrate
                            size_t bitrate_pos = url.find(std::to_string(client_bitrate));
                            if (bitrate_pos != std::string::npos) 
                            {
                                url.replace(bitrate_pos, std::to_string(client_bitrate).length(), std::to_string(selected_bitrate));
                                client_request.replace(client_request.find(request.getUrl()), request.getUrl().length(), url);
                            }
                            spdlog::info("Segment requested by {} forwarded to {}:{} as {} at bitrate {} Kbps", clients_info[i].uuid, config.host_name, config.port, url, selected_bitrate); 
                        }
                        send(server_conn, client_request.c_str(), client_request.size(), 0);
                    }
                    else
                    {
                        send(server_conn, client_request.c_str(), client_request.size(), 0);
                    }
                }
                else if (request.getMethod() == "POST" && request.getUrl() == "/on-fragment-received")
                {
                    std::string client_uuid = request.getHeaderValue("X-489-UUID");
                    long long x_fragment_size = std::stoll(request.getHeaderValue("X-Fragment-Size"));
                    long long x_timestamp_start = std::stoll(request.getHeaderValue("X-Timestamp-Start"));
                    long long x_timestamp_end = std::stoll(request.getHeaderValue("X-Timestamp-End"));
                    long long duration = x_timestamp_end - x_timestamp_start;

                    double tput = (x_fragment_size * 8.0 / 100000.0) / (duration / 1000.0); // in Kbps
                    clients_info[i].avg_throughput = config.alpha * tput + (1 - config.alpha) * clients_info[i].avg_throughput;
                    clients_info[i].uuid = client_uuid;

                    std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
                    send(client_sock, response.c_str(), response.size(), 0);
                    spdlog::info("Client {} finished receiving a segment of size {} bytes in {} ms. Throughput: {} Kbps. Avg Throughput: {} Kbps", client_uuid, x_fragment_size, duration, tput, clients_info[i].avg_throughput);
                    continue;
                }
                else
                {
                    send(server_conn, client_request.c_str(), client_request.size(), 0);
                }

                // std::cout << "Reading response from server:\n";
                std::string server_response = readHttpResponse(server_conn);
                if (server_response.empty()) 
                {
                    spdlog::info("Client socket sockfd {} disconnected", client_sock);
                    close(client_sock);
                    close(server_conn);
                    client_sockets[i] = 0;
                    continue;
                }
                // std::cout << server_response << "\n";

                send(client_sock, server_response.c_str(), server_response.size(), 0);
            }
        }
    }

    return 0;
}
