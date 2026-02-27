/**
 * convertalis-fast - 快速 convertalis 客户端
 *
 * 用法: ./convertalis-fast <result.m8> <output.m8> --socket-path <path>
 *
 * 从 convertserver 获取 target 名称，避免加载大型 lookup 文件
 */

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <cstring>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>

// 连接到 convertserver 的客户端
class ConvertClient {
private:
    int sock;
    std::string socketPath;
    char buffer[1048576];  // 1MB buffer

public:
    ConvertClient(const std::string& path) : sock(-1), socketPath(path) {}

    bool connect() {
        sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) {
            return false;
        }

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(sock);
            sock = -1;
            return false;
        }

        return true;
    }

    // 查询单个 ID
    std::string getName(uint32_t id) {
        std::string request = "GET " + std::to_string(id) + "\n";
        send(sock, request.c_str(), request.size(), 0);

        ssize_t bytesRead = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (bytesRead <= 0) {
            return "ERROR";
        }

        buffer[bytesRead] = '\0';
        std::string response(buffer);

        // 移除换行符
        if (!response.empty() && response.back() == '\n') {
            response.pop_back();
        }

        return response;
    }

    // 批量查询
    std::vector<std::string> getNames(const std::vector<uint32_t>& ids) {
        std::vector<std::string> results;
        results.reserve(ids.size());

        if (ids.empty()) return results;

        // 构建批量请求
        std::string request = "BATCH";
        for (uint32_t id : ids) {
            request += " " + std::to_string(id);
        }
        request += "\n";

        // 确保完整发送
        size_t totalSent = 0;
        while (totalSent < request.size()) {
            ssize_t sent = send(sock, request.c_str() + totalSent, request.size() - totalSent, 0);
            if (sent <= 0) {
                for (size_t i = 0; i < ids.size(); i++) {
                    results.push_back("ERROR");
                }
                return results;
            }
            totalSent += sent;
        }

        // 循环接收直到收到完整响应（以换行符结尾）
        std::string response;
        while (true) {
            ssize_t bytesRead = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (bytesRead <= 0) {
                for (size_t i = 0; i < ids.size(); i++) {
                    results.push_back("ERROR");
                }
                return results;
            }
            buffer[bytesRead] = '\0';
            response += buffer;

            // 检查是否接收完整
            if (!response.empty() && response.back() == '\n') {
                break;
            }
        }

        // 解析响应（\t 分隔）
        size_t pos = 0;
        size_t start = 0;
        while (pos < response.size()) {
            if (response[pos] == '\t' || response[pos] == '\n') {
                results.push_back(response.substr(start, pos - start));
                start = pos + 1;
            }
            pos++;
        }
        if (start < response.size()) {
            results.push_back(response.substr(start));
        }

        return results;
    }

    void close() {
        if (sock >= 0) {
            ::close(sock);
            sock = -1;
        }
    }

    ~ConvertClient() {
        close();
    }
};

// 安全解析函数
double safeStod(const std::string& s, double defaultVal = 0.0) {
    try {
        return std::stod(s);
    } catch (...) {
        return defaultVal;
    }
}

int safeStoi(const std::string& s, int defaultVal = 0) {
    try {
        return std::stoi(s);
    } catch (...) {
        return defaultVal;
    }
}

uint32_t safeStoul(const std::string& s, uint32_t defaultVal = 0) {
    try {
        return std::stoul(s);
    } catch (...) {
        return defaultVal;
    }
}

// 解析 M8 格式的一行
struct AlignmentResult {
    std::string query;
    uint32_t targetId;
    double fident;
    int alnlen;
    int mismatch;
    int gapopen;
    int qstart;
    int qend;
    int tstart;
    int tend;
    double evalue;
    double bits;

    static AlignmentResult parse(const std::string& line) {
        AlignmentResult r;
        r.targetId = 0;
        r.fident = 0;
        r.alnlen = 0;
        r.mismatch = 0;
        r.gapopen = 0;
        r.qstart = 0;
        r.qend = 0;
        r.tstart = 0;
        r.tend = 0;
        r.evalue = 0;
        r.bits = 0;

        std::istringstream iss(line);
        std::string field;

        if (!std::getline(iss, r.query, '\t')) return r;
        if (!std::getline(iss, field, '\t')) return r;
        r.targetId = safeStoul(field);
        if (!std::getline(iss, field, '\t')) return r;
        r.fident = safeStod(field);
        if (!std::getline(iss, field, '\t')) return r;
        r.alnlen = safeStoi(field);
        if (!std::getline(iss, field, '\t')) return r;
        r.mismatch = safeStoi(field);
        if (!std::getline(iss, field, '\t')) return r;
        r.gapopen = safeStoi(field);
        if (!std::getline(iss, field, '\t')) return r;
        r.qstart = safeStoi(field);
        if (!std::getline(iss, field, '\t')) return r;
        r.qend = safeStoi(field);
        if (!std::getline(iss, field, '\t')) return r;
        r.tstart = safeStoi(field);
        if (!std::getline(iss, field, '\t')) return r;
        r.tend = safeStoi(field);
        if (!std::getline(iss, field, '\t')) return r;
        r.evalue = safeStod(field);
        if (!std::getline(iss, field, '\t')) return r;
        r.bits = safeStod(field);

        return r;
    }
};

void printUsage(const char* prog) {
    std::cerr << "Usage: " << prog << " <result.m8> <output.m8> --socket-path <path>" << std::endl;
    std::cerr << std::endl;
    std::cerr << "Options:" << std::endl;
    std::cerr << "  --socket-path <path>  Path to convertserver socket (default: /tmp/convertserver.sock)" << std::endl;
    std::cerr << "  --threads <n>         Number of threads (default: 1)" << std::endl;
    std::cerr << "  --batch-size <n>      Batch size for queries (default: 1000)" << std::endl;
    std::cerr << std::endl;
    std::cerr << "Input format: queryId\\ttargetId\\t..." << std::endl;
    std::cerr << "Output format: queryName\\ttargetName\\t..." << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        printUsage(argv[0]);
        return 1;
    }

    std::string inputFile = argv[1];
    std::string outputFile = argv[2];
    std::string socketPath = "/tmp/convertserver.sock";
    int threads = 1;
    int batchSize = 1000;

    // 解析参数
    for (int i = 3; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--socket-path" && i + 1 < argc) {
            socketPath = argv[++i];
        } else if (arg == "--threads" && i + 1 < argc) {
            threads = std::stoi(argv[++i]);
        } else if (arg == "--batch-size" && i + 1 < argc) {
            batchSize = std::stoi(argv[++i]);
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
    }

    auto startTotal = std::chrono::steady_clock::now();

    // 连接到 convertserver
    ConvertClient client(socketPath);
    if (!client.connect()) {
        std::cerr << "[ERROR] Cannot connect to convertserver at " << socketPath << std::endl;
        std::cerr << "[INFO] Start convertserver first: ./convertserver <lookup_file> " << socketPath << std::endl;
        return 1;
    }

    std::cerr << "[INFO] Connected to convertserver at " << socketPath << std::endl;

    // 打开输入文件
    std::ifstream inFile(inputFile);
    if (!inFile) {
        std::cerr << "[ERROR] Cannot open input file: " << inputFile << std::endl;
        return 1;
    }

    // 打开输出文件
    std::ofstream outFile(outputFile);
    if (!outFile) {
        std::cerr << "[ERROR] Cannot open output file: " << outputFile << std::endl;
        return 1;
    }

    // 第一遍：收集所有 target ID
    std::cerr << "[INFO] Scanning input file..." << std::endl;
    std::unordered_map<uint32_t, std::string> idToName;
    std::vector<std::string> lines;
    std::string line;

    while (std::getline(inFile, line)) {
        if (line.empty() || line[0] == '#') continue;
        lines.push_back(line);

        AlignmentResult r = AlignmentResult::parse(line);
        if (idToName.find(r.targetId) == idToName.end()) {
            idToName[r.targetId] = "";  // 占位
        }
    }
    inFile.close();

    std::cerr << "[INFO] Found " << lines.size() << " alignments, " << idToName.size() << " unique target IDs" << std::endl;

    // 批量获取名称
    std::cerr << "[INFO] Fetching target names from convertserver..." << std::endl;
    auto startFetch = std::chrono::steady_clock::now();

    std::vector<uint32_t> idsToFetch;
    idsToFetch.reserve(idToName.size());
    for (auto& pair : idToName) {
        idsToFetch.push_back(pair.first);
    }

    // 分批查询
    size_t fetched = 0;
    for (size_t i = 0; i < idsToFetch.size(); i += batchSize) {
        size_t end = std::min(i + batchSize, idsToFetch.size());
        std::vector<uint32_t> batch(idsToFetch.begin() + i, idsToFetch.begin() + end);

        std::vector<std::string> names = client.getNames(batch);
        for (size_t j = 0; j < batch.size() && j < names.size(); j++) {
            idToName[batch[j]] = names[j];
        }

        fetched += batch.size();
        if (fetched % 100000 == 0 || fetched == idsToFetch.size()) {
            std::cerr << "[INFO] Fetched " << fetched << "/" << idsToFetch.size() << " names..." << std::endl;
        }
    }

    auto endFetch = std::chrono::steady_clock::now();
    auto fetchTime = std::chrono::duration_cast<std::chrono::milliseconds>(endFetch - startFetch).count();
    std::cerr << "[INFO] Fetch completed in " << fetchTime << "ms" << std::endl;

    client.close();

    // 第二遍：输出结果
    std::cerr << "[INFO] Writing output..." << std::endl;
    auto startWrite = std::chrono::steady_clock::now();

    for (const std::string& line : lines) {
        AlignmentResult r = AlignmentResult::parse(line);

        // 获取 target 名称
        std::string targetName = idToName[r.targetId];
        if (targetName.empty() || targetName == "NOT_FOUND") {
            targetName = std::to_string(r.targetId);
        }

        // 输出格式化结果
        outFile << r.query << "\t"
                << targetName << "\t"
                << std::fixed << std::setprecision(3) << r.fident << "\t"
                << r.alnlen << "\t"
                << r.mismatch << "\t"
                << r.gapopen << "\t"
                << r.qstart << "\t"
                << r.qend << "\t"
                << r.tstart << "\t"
                << r.tend << "\t"
                << std::scientific << std::setprecision(2) << r.evalue << "\t"
                << std::fixed << std::setprecision(1) << r.bits
                << "\n";
    }

    outFile.close();

    auto endWrite = std::chrono::steady_clock::now();
    auto writeTime = std::chrono::duration_cast<std::chrono::milliseconds>(endWrite - startWrite).count();

    auto endTotal = std::chrono::steady_clock::now();
    auto totalTime = std::chrono::duration_cast<std::chrono::milliseconds>(endTotal - startTotal).count();

    std::cerr << "[INFO] Write completed in " << writeTime << "ms" << std::endl;
    std::cerr << "[INFO] Total time: " << totalTime << "ms" << std::endl;
    std::cerr << "[INFO] Output written to: " << outputFile << std::endl;

    return 0;
}
