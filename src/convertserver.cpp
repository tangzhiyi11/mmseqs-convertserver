/**
 * convertserver - 快速 ID→名称 查询服务
 *
 * 用法: ./convertserver <lookup_file> [socket_path]
 *
 * 示例:
 *   ./convertserver /path/to/db.lookup /tmp/convertserver.sock
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <iostream>
#include <fstream>
#include <unordered_map>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstring>
#include <chrono>

// 全局变量
static std::unordered_map<uint32_t, std::string> idToName;
static std::atomic<bool> running(true);

// 信号处理
void signalHandler(int signum) {
    std::cerr << "\n[INFO] Received signal " << signum << ", shutting down..." << std::endl;
    running = false;
}

// 加载 lookup 文件到内存
bool loadLookupFile(const std::string& lookupFile) {
    std::cerr << "[INFO] Loading lookup file: " << lookupFile << std::endl;
    auto start = std::chrono::steady_clock::now();

    int fd = open(lookupFile.c_str(), O_RDONLY);
    if (fd < 0) {
        std::cerr << "[ERROR] Cannot open lookup file: " << lookupFile << std::endl;
        return false;
    }

    // 获取文件大小
    struct stat st;
    fstat(fd, &st);
    off_t fileSize = st.st_size;

    std::cerr << "[INFO] File size: " << (fileSize / 1024.0 / 1024.0 / 1024.0) << " GB" << std::endl;

    // mmap 文件
    void* mapped = mmap(NULL, fileSize, PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
    if (mapped == MAP_FAILED) {
        std::cerr << "[ERROR] mmap failed: " << strerror(errno) << std::endl;
        close(fd);
        return false;
    }

    // 预分配
    size_t estimatedEntries = fileSize / 40;
    idToName.reserve(estimatedEntries);

    char* data = (char*)mapped;
    size_t size = fileSize;

    // 解析每行
    size_t pos = 0;
    size_t lineStart = 0;
    size_t count = 0;
    size_t lastReport = 0;

    while (pos < size) {
        if (data[pos] == '\n') {
            size_t lineLen = pos - lineStart;
            if (lineLen > 0) {
                char* line = data + lineStart;

                // 找第一个 tab
                size_t tab1 = 0;
                while (tab1 < lineLen && line[tab1] != '\t') tab1++;

                // 找第二个 tab
                size_t tab2 = tab1 + 1;
                while (tab2 < lineLen && line[tab2] != '\t') tab2++;

                if (tab1 < lineLen && tab2 < lineLen) {
                    // 解析 ID
                    uint32_t id = 0;
                    for (size_t i = 0; i < tab1; i++) {
                        if (line[i] >= '0' && line[i] <= '9') {
                            id = id * 10 + (line[i] - '0');
                        }
                    }

                    // 解析 Name
                    size_t nameLen = tab2 - tab1 - 1;
                    std::string name(line + tab1 + 1, nameLen);

                    idToName[id] = std::move(name);
                    count++;
                }
            }
            lineStart = pos + 1;

            // 进度报告
            if (count - lastReport > 10000000) {
                std::cerr << "[INFO] Loaded " << (count / 1000000) << "M entries..." << std::endl;
                lastReport = count;
            }
        }
        pos++;
    }

    munmap(mapped, fileSize);
    close(fd);

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::seconds>(end - start).count();

    std::cerr << "[INFO] Loaded " << count << " entries in " << duration << "s" << std::endl;
    std::cerr << "[INFO] Estimated memory: ~" << (count * 50 / 1024.0 / 1024.0 / 1024.0) << " GB" << std::endl;

    return true;
}

// 处理客户端请求
void handleClient(int clientSocket) {
    char buffer[65536];

    while (running) {
        ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (bytesRead <= 0) break;

        buffer[bytesRead] = '\0';
        std::string request(buffer);
        std::string response;

        if (request.substr(0, 4) == "GET ") {
            // 单个查询
            try {
                uint32_t id = std::stoul(request.substr(4));
                auto it = idToName.find(id);
                if (it != idToName.end()) {
                    response = it->second + "\n";
                } else {
                    response = "NOT_FOUND\n";
                }
            } catch (...) {
                response = "ERROR\n";
            }
        } else if (request.substr(0, 6) == "BATCH ") {
            // 批量查询
            size_t pos = 6;
            while (pos < request.size()) {
                while (pos < request.size() && request[pos] == ' ') pos++;
                if (pos >= request.size() || request[pos] == '\n') break;

                size_t start = pos;
                while (pos < request.size() && request[pos] != ' ' && request[pos] != '\n') pos++;

                try {
                    uint32_t id = std::stoul(request.substr(start, pos - start));
                    auto it = idToName.find(id);
                    if (it != idToName.end()) {
                        response += it->second + "\t";
                    } else {
                        response += "NOT_FOUND\t";
                    }
                } catch (...) {
                    response += "ERROR\t";
                }
            }
            if (!response.empty()) {
                response.back() = '\n';
            } else {
                response = "\n";  // 空响应
            }
        } else if (request.substr(0, 4) == "PING") {
            response = "PONG\n";
        } else if (request.substr(0, 4) == "STAT") {
            response = "ENTRIES:" + std::to_string(idToName.size()) + "\n";
        } else {
            response = "ERROR:Unknown command\n";
        }

        send(clientSocket, response.c_str(), response.size(), 0);
    }

    close(clientSocket);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <lookup_file> [socket_path]" << std::endl;
        return 1;
    }

    std::string lookupFile = argv[1];
    std::string socketPath = lookupFile + ".sock";
    if (argc > 2) {
        socketPath = argv[2];
    }

    // 信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // 加载 lookup
    if (!loadLookupFile(lookupFile)) {
        return 1;
    }

    // 创建 socket
    int serverSocket = socket(AF_UNIX, SOCK_STREAM, 0);
    if (serverSocket < 0) {
        std::cerr << "[ERROR] Cannot create socket" << std::endl;
        return 1;
    }

    // 删除旧的 socket 文件
    unlink(socketPath.c_str());

    // 绑定
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(serverSocket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "[ERROR] Cannot bind socket to " << socketPath << std::endl;
        close(serverSocket);
        return 1;
    }

    // 监听
    if (listen(serverSocket, 10) < 0) {
        std::cerr << "[ERROR] Cannot listen on socket" << std::endl;
        close(serverSocket);
        unlink(socketPath.c_str());
        return 1;
    }

    std::cerr << "[INFO] convertserver started" << std::endl;
    std::cerr << "[INFO] Socket path: " << socketPath << std::endl;
    std::cerr << "[INFO] Ready to accept connections" << std::endl;
    std::cout << socketPath << std::endl;

    // 主循环
    while (running) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(serverSocket, &readfds);

        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int activity = select(serverSocket + 1, &readfds, NULL, NULL, &timeout);

        if (activity < 0 && errno != EINTR) {
            std::cerr << "[ERROR] Select error" << std::endl;
            break;
        }

        if (activity > 0 && FD_ISSET(serverSocket, &readfds)) {
            int clientSocket = accept(serverSocket, NULL, NULL);
            if (clientSocket < 0) {
                continue;
            }

            std::thread clientThread(handleClient, clientSocket);
            clientThread.detach();
        }
    }

    // 清理
    close(serverSocket);
    unlink(socketPath.c_str());

    std::cerr << "[INFO] convertserver stopped" << std::endl;

    return 0;
}
