#pragma once

#include <string>
#include <vector>

namespace mmdb::dbio {

struct BoxTable {
    struct Col {
        int fieldIdx = -1;
        std::string nameUpper;
        std::string comment;
        bool isString = false;
    };

    std::string banner;
    std::vector<int> rows; // 1-based
    std::vector<Col> cols;

    int maxColWStr = 24;
    int maxColWNum = 10;
    int idxWCap = 6;

    // Comment header may be longer than the column width; wrap it to multiple lines.
    // Keep it small (2~3) to avoid exploding table height.
    int maxCommentLines = 3;

    void render() const;
};

} // namespace mmdb::dbio
