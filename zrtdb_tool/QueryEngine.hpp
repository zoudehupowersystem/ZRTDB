// ZRTDB（Zero-copy Real-Time Data Bus）
// Copyright (c) 2026 邹德虎 （Zou Dehu）
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "DataAccessor.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mmdb::dbio {

// 一个“轻量表达式引擎”，用于 zrtdb_tool 的查询/定位/排序。
// 目标：不引入 SQL 复杂度，只提供足够好用的布尔筛选表达式。
// 约定：
// - 字段名大小写不敏感（内部统一转大写）
// - 支持 _IDX 伪字段（1-based 行号）
// - 支持操作符：= == != < <= > >= ~ !~ AND OR NOT (以及 && || !)
// - ~ / !~：通配符匹配（* ?），若 pattern 不含通配符则视为“子串包含”
// - 字符串字面量支持单/双引号
class QueryEngine {
public:
    struct EvalLookup {
        // 输入 identUpper（已大写）与 row（1-based），输出 DbValue
        // 返回 false 表示未找到该标识符（会被当作 NULL）
        std::function<bool(const std::string& identUpper, int row, DbValue& out)> resolve;
    };

    struct CompileResult {
        std::function<bool(int row)> eval; // row 为 1-based
        std::vector<std::string> identifiers_upper; // 解析到的标识符集合（去重）
    };

    // 编译布尔表达式。成功返回 true，否则 errMsg 给出原因。
    static bool compileBoolExpr(const std::string& expr,
        const EvalLookup& lookup,
        CompileResult& out,
        std::string& errMsg);

    // 将命令行按空白切分，但保留引号内容（引号本身也会保留）。
    static std::vector<std::string> splitCommandTokens(const std::string& line);

    // 是否包含通配符（用于 ~ 语义）
    static bool hasGlob(std::string_view pattern);

    // case-insensitive 通配符匹配（* ?）
    static bool globMatchCI(std::string_view pattern, std::string_view text);

    // case-insensitive 子串包含
    static bool containsCI(std::string_view text, std::string_view needle);

private:
    QueryEngine() = default;
};

} // namespace mmdb::dbio
