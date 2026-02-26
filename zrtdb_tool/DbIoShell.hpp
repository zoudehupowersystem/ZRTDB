// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace mmdb::dbio {

struct ShellContext {
    int dbIdx = -1; // current DB index
    int recIdx = -1; // current record index (0-based into record_ids)
    int currentSlot = 1; // 1-based row pointer
    std::string dbName;
    std::string recName;
    bool isGlobal = true; // ITEM mode
};

struct TableViewState {
    bool active = false;
    enum Kind { NONE,
        RECORD,
        GLOBALS } kind
        = NONE;
    int record_index = -1; // 0-based
    int row0 = 1; // 1-based
    int col0 = 0; // 0-based
    std::vector<int> field_indices; // indexes into g_runtime_app.field_ids
};

class DbIoShell {
public:
    explicit DbIoShell(const std::string& appUpper, const std::string& startDbUpper);
    void run();

private:
    // 新增：快照命令
    void cmdSnap(const std::string& args);
    void cmdListSnap(const std::string& args);
    void cmdLoadSnap(const std::string& args);

    std::string appUpper_; // 新增：保存 APP 名
    static constexpr int kPageRows = 10;
    static constexpr int kPageCols = 10;

    void initCommands();
    void processLine(const std::string& lineRaw);

    // Commands
    void cmdHelp(const std::string&);
    void cmdQuit(const std::string&);
    void cmdListGlobals(const std::string&);
    void cmdListRecords(const std::string&);
    void cmdPosition(const std::string&);
    void cmdShowCurrent(const std::string&);
    void cmdInspectField(const std::string&);
    void cmdShow(const std::string&);

    // Query / Locate / Sort
    void cmdSelect(const std::string&);
    void cmdFind(const std::string&);
    void cmdNext(const std::string&);
    void cmdPrev(const std::string&);

    void cmdStatus(const std::string&);

    // View
    bool handleViewKey(const std::string& line);
    void renderGlobalsTable();
    void renderRecordTable();

    // Helpers
    bool switchToDb(const std::string& dbUpper);
    bool switchToRecord(const std::string& recUpper);
    int findRecordIndexInDb(int dbIdx, const std::string& recUpper) const;
    int findFieldIndexGlobalOrCurrent(const std::string& fieldUpper) const;
    bool tryAssignSimple(const std::string& lineUpper);

    int getLVForRecordGlobalIndex(int recGlobalIdx) const;

    // mapping helpers
    void mapAllPartitionsForDb(int dbIdx);
    void mapPartitionByIndex(int prtIdx);

    ShellContext ctx_;
    TableViewState view_;
    bool running_ = true;
    struct QueryNavState {
        bool active = false;
        int dbIdx = -1;
        int recIdx = -1;
        bool isGlobal = true;
        std::vector<int> rows; // 1-based row indices
        int pos = -1; // current index in rows
    };

    QueryNavState qnav_;

    std::map<std::string, std::function<void(std::string)>> commands_;
};

} // namespace mmdb::dbio
