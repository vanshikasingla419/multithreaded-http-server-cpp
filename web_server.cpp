#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <cstring>
#include <ctime>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

const int PORT = 8080;
const int THREAD_POOL_SIZE = 10;

// ============================================================================
// METRICS & LOGGING
// ============================================================================
struct Metrics {
    DWORD total_requests;
    DWORD successful_requests;
    DWORD failed_requests;
    DWORD total_bytes_sent;
    time_t start_time;
    CRITICAL_SECTION cs;
} g_metrics = {0, 0, 0, 0, 0};

void init_metrics() {
    InitializeCriticalSection(&g_metrics.cs);
    g_metrics.total_requests = 0;
    g_metrics.successful_requests = 0;
    g_metrics.failed_requests = 0;
    g_metrics.total_bytes_sent = 0;
    g_metrics.start_time = time(NULL);
}

void log_request(int status_code, const std::string& path, int bytes_sent) {
    EnterCriticalSection(&g_metrics.cs);
    g_metrics.total_requests++;
    if (status_code >= 200 && status_code < 400) {
        g_metrics.successful_requests++;
    } else {
        g_metrics.failed_requests++;
    }
    g_metrics.total_bytes_sent += bytes_sent;
    LeaveCriticalSection(&g_metrics.cs);
    
    // Console logging with timestamp
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char time_str[20];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    std::cout << "[" << time_str << "] " << status_code << " " << path << " (" << bytes_sent << " bytes)\n";
}

std::string get_metrics_json() {
    EnterCriticalSection(&g_metrics.cs);
    time_t now = time(NULL);
    DWORD uptime = (DWORD)(now - g_metrics.start_time);
    DWORD total = g_metrics.total_requests;
    DWORD successful = g_metrics.successful_requests;
    DWORD failed = g_metrics.failed_requests;
    DWORD bytes = g_metrics.total_bytes_sent;
    LeaveCriticalSection(&g_metrics.cs);
    
    std::stringstream ss;
    ss << "{\"total_requests\":" << total
       << ",\"successful_requests\":" << successful
       << ",\"failed_requests\":" << failed
       << ",\"total_bytes_sent\":" << bytes
       << ",\"uptime_seconds\":" << uptime
       << ",\"average_response_size\":" << (total > 0 ? bytes / total : 0) << "}";
    return ss.str();
}

// ============================================================================
// REQUEST HANDLERS
// ============================================================================
void handle_client(SOCKET client_socket);
void serve_static_file(SOCKET client_socket, std::string target_path);
void send_json_response(SOCKET client_socket, const std::string& json_data, int status_code = 200);
void send_error_response(SOCKET client_socket, int status_code, std::string status_text);
void send_http_response(SOCKET client_socket, int status_code, const std::string& content_type, const std::string& body);

void send_http_response(SOCKET client_socket, int status_code, const std::string& content_type, const std::string& body) {
    std::string status_text;
    if (status_code == 200) status_text = "OK";
    else if (status_code == 404) status_text = "Not Found";
    else if (status_code == 501) status_text = "Not Implemented";
    else status_text = "Error";
    
    std::string headers = "HTTP/1.1 " + std::to_string(status_code) + " " + status_text + "\r\n";
    headers += "Content-Type: " + content_type + "\r\n";
    headers += "Content-Length: " + std::to_string(body.length()) + "\r\n";
    headers += "Connection: close\r\n\r\n";
    
    send(client_socket, headers.c_str(), (int)headers.length(), 0);
    send(client_socket, body.c_str(), (int)body.length(), 0);
}

void handle_client(SOCKET client_socket) {
    char buffer[4096] = {0};
    int bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    if (bytes_read <= 0) {
        closesocket(client_socket);
        return;
    }

    std::string request(buffer);
    std::istringstream request_stream(request);
    std::string http_method, target_path;
    request_stream >> http_method >> target_path;

    int status_code = 404;
    int response_bytes = 0;

    if (http_method != "GET") {
        send_error_response(client_socket, 501, "Not Implemented");
        status_code = 501;
        response_bytes = 0;
    }
    else if (target_path == "/") {
        serve_static_file(client_socket, "/index.html");
        status_code = 200;
        response_bytes = 0;
    }
    else if (target_path == "/api/status") {
        std::string json = "{\"status\":\"running\",\"engine\":\"Advanced C++ Server\"}";
        send_json_response(client_socket, json, 200);
        status_code = 200;
        response_bytes = json.length();
    }
    else if (target_path == "/api/metrics") {
        std::string json = get_metrics_json();
        send_json_response(client_socket, json, 200);
        status_code = 200;
        response_bytes = json.length();
    }
    else if (target_path == "/api/health") {
        std::string json = "{\"health\":\"ok\",\"timestamp\":" + std::to_string(time(NULL)) + "}";
        send_json_response(client_socket, json, 200);
        status_code = 200;
        response_bytes = json.length();
    }
    else {
        serve_static_file(client_socket, target_path);
        status_code = 404;
        response_bytes = 0;
    }

    log_request(status_code, target_path, response_bytes);
    closesocket(client_socket);
}

void serve_static_file(SOCKET client_socket, std::string target_path) {
    if (target_path == "/") {
        target_path = "/index.html";
    }

    std::string file_local_path = "." + target_path;
    std::ifstream file(file_local_path, std::ios::binary);

    if (file) {
        std::stringstream file_buffer;
        file_buffer << file.rdbuf();
        std::string contents = file_buffer.str();

        std::string content_type = "text/html";
        if (target_path.find(".css") != std::string::npos) content_type = "text/css";
        else if (target_path.find(".js") != std::string::npos) content_type = "application/javascript";
        else if (target_path.find(".json") != std::string::npos) content_type = "application/json";
        else if (target_path.find(".png") != std::string::npos) content_type = "image/png";
        else if (target_path.find(".jpg") != std::string::npos) content_type = "image/jpeg";

        send_http_response(client_socket, 200, content_type, contents);
    } else {
        send_error_response(client_socket, 404, "Not Found");
    }
}

void send_json_response(SOCKET client_socket, const std::string& json_data, int status_code) {
    send_http_response(client_socket, status_code, "application/json", json_data);
}

void send_error_response(SOCKET client_socket, int status_code, std::string status_text) {
    std::string body = "<html><body><h1>" + std::to_string(status_code) + " " + status_text + "</h1></body></html>";
    send_http_response(client_socket, status_code, "text/html", body);
}

// ============================================================================
// THREAD POOL WORKER
// ============================================================================
CRITICAL_SECTION g_socket_queue_cs;
SOCKET g_socket_queue[1000];
int g_socket_queue_count = 0;
HANDLE g_queue_event;
volatile bool g_running = true;

DWORD WINAPI worker_thread(LPVOID param) {
    while (g_running) {
        WaitForSingleObject(g_queue_event, 100);
        
        EnterCriticalSection(&g_socket_queue_cs);
        if (g_socket_queue_count > 0) {
            SOCKET client_socket = g_socket_queue[0];
            for (int i = 0; i < g_socket_queue_count - 1; i++) {
                g_socket_queue[i] = g_socket_queue[i + 1];
            }
            g_socket_queue_count--;
            LeaveCriticalSection(&g_socket_queue_cs);
            
            handle_client(client_socket);
        } else {
            LeaveCriticalSection(&g_socket_queue_cs);
        }
    }
    return 0;
}

void enqueue_socket(SOCKET sock) {
    EnterCriticalSection(&g_socket_queue_cs);
    if (g_socket_queue_count < 1000) {
        g_socket_queue[g_socket_queue_count++] = sock;
        SetEvent(g_queue_event);
    }
    LeaveCriticalSection(&g_socket_queue_cs);
}

// ============================================================================
// MAIN SERVER APPLICATION
// ============================================================================
int main() {
    // Initialize Winsock2
    WSADATA wsa_data;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (result != 0) {
        std::cerr << "WSAStartup failed with error: " << result << "\n";
        return 1;
    }

    // Initialize metrics and synchronization
    init_metrics();
    InitializeCriticalSection(&g_socket_queue_cs);
    g_queue_event = CreateEvent(NULL, FALSE, FALSE, NULL);

    // Create thread pool
    HANDLE threads[THREAD_POOL_SIZE];
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        threads[i] = CreateThread(NULL, 0, worker_thread, NULL, 0, NULL);
    }

    // Create socket
    SOCKET server_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_fd == INVALID_SOCKET) {
        std::cerr << "Socket creation failed.\n";
        WSACleanup();
        return 1;
    }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Port binding failed.\n";
        closesocket(server_fd);
        WSACleanup();
        return 1;
    }

    if (listen(server_fd, 10) < 0) {
        std::cerr << "Listen failed.\n";
        closesocket(server_fd);
        WSACleanup();
        return 1;
    }

    std::cout << "===================================================\n";
    std::cout << "   Advanced C++ Multi-Threaded Web Server\n";
    std::cout << "===================================================\n";
    std::cout << "Starting on port " << PORT << "...\n";
    std::cout << "Thread pool size: " << THREAD_POOL_SIZE << "\n";
    std::cout << "Open browser: http://localhost:" << PORT << "\n";
    std::cout << "API endpoints:\n";
    std::cout << "  - http://localhost:" << PORT << "/api/status\n";
    std::cout << "  - http://localhost:" << PORT << "/api/metrics\n";
    std::cout << "  - http://localhost:" << PORT << "/api/health\n";
    std::cout << "Press Ctrl+C to shutdown...\n";
    std::cout << "===================================================\n\n";

    while (g_running) {
        int addrlen = sizeof(address);
        SOCKET client_socket = accept(server_fd, (struct sockaddr*)&address, &addrlen);
        if (client_socket == INVALID_SOCKET) {
            continue;
        }
        enqueue_socket(client_socket);
    }

    // Cleanup
    g_running = false;
    Sleep(500);
    
    for (int i = 0; i < THREAD_POOL_SIZE; i++) {
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
    }

    CloseHandle(g_queue_event);
    DeleteCriticalSection(&g_socket_queue_cs);
    DeleteCriticalSection(&g_metrics.cs);
    closesocket(server_fd);
    WSACleanup();
    
    std::cout << "\nServer shutdown complete.\n";
    return 0;
}