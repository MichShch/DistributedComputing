#include "formatters.hpp"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace dc {
namespace cli {

namespace {

std::vector<std::size_t> ComputeWidths(const std::vector<std::string>& headers,
                                       const std::vector<std::vector<std::string>>& rows) {
    std::vector<std::size_t> widths(headers.size(), 0);
    for (std::size_t i = 0; i < headers.size(); ++i) {
        widths[i] = headers[i].size();
    }
    for (const auto& row : rows) {
        for (std::size_t i = 0; i < row.size() && i < widths.size(); ++i) {
            widths[i] = std::max(widths[i], row[i].size());
        }
    }
    return widths;
}

void PrintRow(std::ostream& out,
              const std::vector<std::string>& row,
              const std::vector<std::size_t>& widths) {
    for (std::size_t i = 0; i < widths.size(); ++i) {
        std::string value = (i < row.size()) ? row[i] : "";
        out << std::left << std::setw(static_cast<int>(widths[i])) << value;
        if (i + 1 < widths.size()) {
            out << "  ";
        }
    }
    out << '\n';
}

void PrintSeparator(std::ostream& out, const std::vector<std::size_t>& widths) {
    for (std::size_t i = 0; i < widths.size(); ++i) {
        out << std::string(widths[i], '-');
        if (i + 1 < widths.size()) {
            out << "  ";
        }
    }
    out << '\n';
}

}  // namespace

void PrintTable(std::ostream& out,
                const std::vector<std::string>& headers,
                const std::vector<std::vector<std::string>>& rows) {
    if (headers.empty()) {
        return;
    }
    auto widths = ComputeWidths(headers, rows);
    PrintRow(out, headers, widths);
    PrintSeparator(out, widths);
    for (const auto& row : rows) {
        PrintRow(out, row, widths);
    }
}

void PrintKeyValueTable(std::ostream& out,
                        const std::vector<std::pair<std::string, std::string>>& rows) {
    std::vector<std::string> headers = {"Field", "Value"};
    std::vector<std::vector<std::string>> table_rows;
    table_rows.reserve(rows.size());
    for (const auto& entry : rows) {
        table_rows.push_back({entry.first, entry.second});
    }
    PrintTable(out, headers, table_rows);
}

}  // namespace cli
}  // namespace dc
