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

            int server_conn = socket(AF_INET, SOCK_STREAM, 0);
            struct sockaddr_in server_addr;
            server_addr.sin_family = AF_INET;
            server_addr.sin_port = htons(config.port);
            struct hostent* server = gethostbyname(config.host_name.c_str());
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





/*#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <sstream>
#include <cmath>
#include <spdlog/spdlog.h>
#include <cxxopts.hpp>

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

class HttpMessage {
public:
    std::string method;
    std::string uri;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;
    
    void parseHeaders(const std::string& headerSection) {
        std::istringstream stream(headerSection);
        std::string line;
        
        // First line is request/status line
        if (std::getline(stream, line)) {
            if (line.back() == '\r') line.pop_back();
            std::istringstream lineStream(line);
            lineStream >> method >> uri >> version;
        }
        
        // Parse headers
        while (std::getline(stream, line)) {
            if (line.back() == '\r') line.pop_back();
            if (line.empty()) break;
            
            size_t colon = line.find(':');
            if (colon != std::string::npos) {
                std::string key = line.substr(0, colon);
                std::string value = line.substr(colon + 1);
                
                // Trim whitespace
                value.erase(0, value.find_first_not_of(" \t"));
                value.erase(value.find_last_not_of(" \t") + 1);
                
                // Convert key to lowercase for case-insensitive comparison
                std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                headers[key] = value;
            }
        }
    }
    
    std::string getHeader(const std::string& key) const {
        std::string lowerKey = key;
        std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(), ::tolower);
        auto it = headers.find(lowerKey);
        return (it != headers.end()) ? it->second : "";
    }
    
    int getContentLength() const {
        std::string cl = getHeader("content-length");
        return cl.empty() ? 0 : std::stoi(cl);
    }
    
    std::string toString() const {
        std::ostringstream oss;
        oss << method << " " << uri << " " << version << "\r\n";
        for (const auto& header : headers) {
            oss << header.first << ": " << header.second << "\r\n";
        }
        oss << "\r\n";
        if (!body.empty()) {
            oss << body;
        }
        return oss.str();
    }
};

struct ClientInfo {
    int client_fd;
    int server_fd;
    std::string client_ip;
    int client_port;
    std::string server_ip;
    int server_port;
    std::string uuid;
    double avg_throughput = 0.0;
    std::string current_video_path;
};

struct VideoInfo {
    std::vector<int> bitrates;
};

std::map<int, ClientInfo> clients; // client_fd -> ClientInfo
std::map<std::string, ClientInfo*> uuid_to_client; // uuid -> ClientInfo
std::map<std::string, VideoInfo> video_cache; // video path -> available bitrates

Config parseArgs(int argc, char** argv) {
    cxxopts::Options options("miProxy", "Adaptive video streaming proxy");
    
    options.add_options()
        ("b,balance", "Enable load balancing")
        ("l,listen-port", "Listen port", cxxopts::value<int>())
        ("h,hostname", "Hostname", cxxopts::value<std::string>())
        ("p,port", "Port", cxxopts::value<int>())
        ("a,alpha", "Alpha value", cxxopts::value<double>())
        ("help", "Print help");
    
    auto result = options.parse(argc, argv);
    
    Config config;
    config.balance = result["balance"].as<bool>();
    
    if (!result.count("listen-port") || !result.count("hostname") || 
        !result.count("port") || !result.count("alpha")) {
        std::cerr << "Error: missing required arguments\n";
        exit(1);
    }
    
    config.listen_port = result["listen-port"].as<int>();
    config.host_name = result["hostname"].as<std::string>();
    config.port = result["port"].as<int>();
    config.alpha = result["alpha"].as<double>();
    
    if (config.port < 1024 || config.port > 65535) {
        std::cerr << "Error: port must be in range [1024, 65535]\n";
        exit(1);
    }
    
    if (config.alpha < 0.0 || config.alpha > 1.0) {
        std::cerr << "Error: alpha must be in range [0.0, 1.0]\n";
        exit(1);
    }
    
    return config;
}

int connectToServer(const std::string& hostname, int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) return -1;
    
    struct hostent* server = gethostbyname(hostname.c_str());
    if (!server) {
        close(sockfd);
        return -1;
    }
    
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    server_addr.sin_port = htons(port);
    
    if (connect(sockfd, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sockfd);
        return -1;
    }
    
    return sockfd;
}

std::string readHttpMessage(int sockfd) {
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
    
    // Parse to get content length
    HttpMessage msg;
    msg.parseHeaders(headers);
    int content_length = msg.getContentLength();
    
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

bool isManifestRequest(const std::string& uri) {
    return uri.find(".mpd") != std::string::npos;
}

bool isVideoSegmentRequest(const std::string& uri) {
    return uri.find("/video/") != std::string::npos && uri.find(".m4s") != std::string::npos;
}

std::string getVideoPath(const std::string& uri) {
    size_t pos = uri.find("/video/");
    if (pos == std::string::npos) return "";
    return uri.substr(0, pos);
}

int selectBitrate(double avg_throughput, const std::vector<int>& bitrates) {
    if (bitrates.empty()) return 0;
    
    double threshold = avg_throughput / 1.5;
    int selected = bitrates[0];
    
    for (int bitrate : bitrates) {
        if (bitrate <= threshold) {
            selected = bitrate;
        }
    }
    
    return selected;
}

std::string modifySegmentRequest(const std::string& uri, int bitrate) {
    // Find vid-XXX-seg pattern
    size_t vid_pos = uri.find("vid-");
    if (vid_pos == std::string::npos) return uri;
    
    size_t seg_pos = uri.find("-seg-", vid_pos);
    if (seg_pos == std::string::npos) return uri;
    
    std::string prefix = uri.substr(0, vid_pos);
    std::string suffix = uri.substr(seg_pos);
    
    return prefix + "vid-" + std::to_string(bitrate) + suffix;
}

int main(int argc, char **argv)
{
    Config config = parseArgs(argc, argv);
    
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        spdlog::error("Failed to create socket");
        return 1;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(config.listen_port);
    
    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        spdlog::error("Bind failed");
        return 1;
    }
    
    if (listen(server_fd, 10) < 0) {
        spdlog::error("Listen failed");
        return 1;
    }
    
    spdlog::info("miProxy started");
    
    fd_set readfds;
    int max_fd = server_fd;
    
    while (true) {
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        max_fd = server_fd;
        
        for (const auto& pair : clients) {
            FD_SET(pair.first, &readfds);
            if (pair.second.server_fd > 0) {
                FD_SET(pair.second.server_fd, &readfds);
                max_fd = std::max(max_fd, pair.second.server_fd);
            }
            max_fd = std::max(max_fd, pair.first);
        }
        
        if (select(max_fd + 1, &readfds, NULL, NULL, NULL) < 0) {
            spdlog::error("Select failed");
            continue;
        }
        
        // New connection
        if (FD_ISSET(server_fd, &readfds)) {
            struct sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
            
            if (client_fd >= 0) {
                ClientInfo info;
                info.client_fd = client_fd;
                info.client_ip = inet_ntoa(client_addr.sin_addr);
                info.client_port = ntohs(client_addr.sin_port);
                info.server_ip = config.host_name;
                info.server_port = config.port;
                
                // Connect to server
                info.server_fd = connectToServer(config.host_name, config.port);
                
                clients[client_fd] = info;
                
                spdlog::info("New client socket connected with {}:{} on sockfd {}", 
                    info.client_ip, info.client_port, client_fd);
            }
        }
        
        // Handle client requests
        std::vector<int> to_remove;
        for (auto& pair : clients) {
            int client_fd = pair.first;
            ClientInfo& info = pair.second;
            
            if (FD_ISSET(client_fd, &readfds)) {
                std::string request_str = readHttpMessage(client_fd);
                
                if (request_str.empty()) {
                    to_remove.push_back(client_fd);
                    continue;
                }

                HttpMessage request;
                request.parseHeaders(request_str);
                
                // Extract UUID if present
                std::string uuid = request.getHeader("x-489-uuid");
                if (!uuid.empty()) {
                    info.uuid = uuid;
                    uuid_to_client[uuid] = &info;
                }
                
                // Handle POST to /on-fragment-received
                if (request.method == "POST" && request.uri == "/on-fragment-received") {
                    long long size = std::stoll(request.getHeader("x-fragment-size"));
                    long long start = std::stoll(request.getHeader("x-timestamp-start"));
                    long long end = std::stoll(request.getHeader("x-timestamp-end"));
                    long long duration = end - start;
                    
                    double tput = (size * 8.0 / 1000.0) / (duration / 1000.0);
                    
                    if (info.avg_throughput == 0.0) {
                        info.avg_throughput = tput;
                    } else {
                        info.avg_throughput = config.alpha * tput + (1 - config.alpha) * info.avg_throughput;
                    }
                    
                    spdlog::info("Client {} finished receiving a segment of size {} bytes in {} ms. Throughput: {} Kbps. Avg Throughput: {} Kbps",
                        uuid, size, duration, (int)tput, (int)info.avg_throughput);
                    
                    std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
                    send(client_fd, response.c_str(), response.size(), 0);
                    continue;
                }
                
                // Handle manifest request
                if (isManifestRequest(request.uri)) {
                    std::string video_path = getVideoPath(request.uri);
                    
                    // Request no-list version for client
                    HttpMessage modified_request = request;
                    size_t mpd_pos = modified_request.uri.find(".mpd");
                    modified_request.uri = modified_request.uri.substr(0, mpd_pos) + "-no-list.mpd";
                    
                    std::string modified_str = modified_request.toString();
                    send(info.server_fd, modified_str.c_str(), modified_str.size(), 0);
                    
                    // spdlog::info("Manifest requested by {} forwarded to {}:{} for {}",
                    //     uuid, info.server_ip, info.server_port, modified_request.uri);
                    
                    // If first time, also request regular manifest for parsing
                    if (video_cache.find(video_path) == video_cache.end()) {
                        // Parse bitrates from manifest (simplified - you should use pugixml)
                        VideoInfo vinfo;
                        vinfo.bitrates = {500, 800, 1100}; // Example bitrates
                        video_cache[video_path] = vinfo;
                    }
                    
                    continue;
                }
                
                // Handle video segment request
                if (isVideoSegmentRequest(request.uri)) {
                    std::string video_path = getVideoPath(request.uri);
                    auto it = video_cache.find(video_path);
                    
                    int selected_bitrate = 500; // default
                    if (it != video_cache.end() && !it->second.bitrates.empty()) {
                        selected_bitrate = selectBitrate(info.avg_throughput, it->second.bitrates);
                    }
                    
                    HttpMessage modified_request = request;
                    modified_request.uri = modifySegmentRequest(request.uri, selected_bitrate);
                    
                    std::string modified_str = modified_request.toString();
                    send(info.server_fd, modified_str.c_str(), modified_str.size(), 0);
                    
                    spdlog::info("Segment requested by {} forwarded to {}:{} as {} at bitrate {} Kbps {}",
                        uuid, info.server_ip, info.server_port, modified_request.uri, selected_bitrate, info.avg_throughput);
                    
                    continue;
                }
                
                // Forward other requests as-is
                send(info.server_fd, request_str.c_str(), request_str.size(), 0);
            }
            
            // Handle server responses
            if (info.server_fd > 0 && FD_ISSET(info.server_fd, &readfds)) {
                std::string response = readHttpMessage(info.server_fd);
                
                if (response.empty()) {
                    to_remove.push_back(client_fd);
                } else {
                    send(client_fd, response.c_str(), response.size(), 0);
                }
            }
        }
        
        // Clean up disconnected clients
        for (int fd : to_remove) {
            auto it = clients.find(fd);
            if (it != clients.end()) {
                spdlog::info("Client socket sockfd {} disconnected", fd);
                if (it->second.server_fd > 0) close(it->second.server_fd);
                close(fd);
                clients.erase(it);
            }
        }
    }
    
    return 0;
}*/