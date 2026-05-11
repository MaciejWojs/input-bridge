#include "linux_wl_clipboard.hpp"

#include <cstdio>
#include <sstream>

namespace {
bool RunWriteCommand(const char* command, const std::string& payload) {
    FILE* pipe = popen(command, "w");
    if (!pipe) {
        return false;
    }
    const size_t bytes = fwrite(payload.data(), 1, payload.size(), pipe);
    return pclose(pipe) == 0 && bytes == payload.size();
}

std::optional<std::string> RunReadCommand(const char* command) {
    FILE* pipe = popen(command, "r");
    if (!pipe) {
        return std::nullopt;
    }

    std::string out;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        out += buffer;
    }
    const int status = pclose(pipe);
    if (status != 0 && out.empty()) {
        return std::nullopt;
    }
    return out;
}
} // namespace

bool LinuxWlClipboard::IsAvailable() const {
    return RunReadCommand("wl-paste --list-types 2>/dev/null").has_value();
}

bool LinuxWlClipboard::SetText(const std::string& text) const {
    return RunWriteCommand("wl-copy --type text/plain;charset=utf-8", text);
}

std::optional<std::string> LinuxWlClipboard::GetText() const {
    return RunReadCommand("wl-paste --no-newline --type text/plain;charset=utf-8 2>/dev/null");
}

bool LinuxWlClipboard::SetFiles(const std::vector<std::string>& files) const {
    std::string payload;
    for (const auto& file : files) {
        payload += "file://" + file + "\r\n";
    }
    return RunWriteCommand("wl-copy --type text/uri-list", payload);
}

std::optional<std::vector<std::string>> LinuxWlClipboard::GetFiles() const {
    auto data = RunReadCommand("wl-paste --no-newline --type text/uri-list 2>/dev/null");
    if (!data || data->empty()) {
        return std::nullopt;
    }

    std::vector<std::string> files;
    std::istringstream stream(*data);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string prefix = "file://";
        if (line.compare(0, prefix.size(), prefix) == 0) {
            files.push_back(line.substr(prefix.size()));
        }
    }
    if (files.empty()) {
        return std::nullopt;
    }
    return files;
}
