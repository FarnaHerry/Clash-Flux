#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace clashflux::ui {

// 空搜索直接映射原列表；筛选结果共享持有，复制虚拟列表 factory 时不复制索引数组。
struct FilteredIndices {
    std::size_t sourceCount = 0;
    std::shared_ptr<const std::vector<std::size_t>> matches;

    std::size_t Size() const { return matches ? matches->size() : sourceCount; }
    std::size_t SourceIndex(std::size_t index) const { return matches ? (*matches)[index] : index; }
};

class FilteredIndicesCache {
public:
    template <class Match> FilteredIndices Resolve(std::uint64_t revision, std::size_t count,
                                                   std::string_view query, Match&& match) {
        if (initialized_ && revision_ == revision && result_.sourceCount == count && query_ == query)
            return result_;
        revision_ = revision;
        query_ = query;
        initialized_ = true;
        result_ = {.sourceCount = count};
        if (!query.empty()) {
            auto indices = std::make_shared<std::vector<std::size_t>>();
            for (std::size_t index = 0; index < count; ++index) {
                if (match(index)) indices->push_back(index);
            }
            result_.matches = std::move(indices);
        }
        return result_;
    }

private:
    bool initialized_ = false;
    std::uint64_t revision_ = 0;
    std::string query_;
    FilteredIndices result_;
};

} // namespace clashflux::ui
