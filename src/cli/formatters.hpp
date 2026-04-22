#pragma once

#include <iosfwd>
#include <string>
#include <utility>
#include <vector>

namespace dc {
namespace cli {

void PrintTable(std::ostream& out,
                const std::vector<std::string>& headers,
                const std::vector<std::vector<std::string>>& rows);

void PrintKeyValueTable(std::ostream& out,
                        const std::vector<std::pair<std::string, std::string>>& rows);

}  // namespace cli
}  // namespace dc
