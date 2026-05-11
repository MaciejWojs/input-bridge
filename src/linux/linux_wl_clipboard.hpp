#pragma once

#include <optional>
#include <string>
#include <vector>

class LinuxWlClipboard {
public:
    bool IsAvailable() const;

    bool SetText(const std::string& text) const;
    std::optional<std::string> GetText() const;

    bool SetFiles(const std::vector<std::string>& files) const;
    std::optional<std::vector<std::string>> GetFiles() const;
};
