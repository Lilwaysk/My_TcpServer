#include "util/Config.h"

#include <cctype>
#include <fstream>
#include <stdexcept>

// 本文件实现 Config 的加载与查询：
//   1. load() 逐行解析 "key = value" 格式的配置文件；
//   2. 解析结果保存在 Config::values_ 中；
//   3. has()/get()/getInt() 只在内存中查找，不再访问文件。

namespace {

// 去掉字符串两端的空白字符（空格、制表符、回车等）。
std::string trim(const std::string& input)
{
    // begin 从左侧出发，跳过所有空白字符。
    std::string::size_type begin = 0;
    while (begin < input.size() && std::isspace(static_cast<unsigned char>(input[begin])))
        ++begin;

    // end 从右侧出发，跳过所有空白字符。
    // 条件 end > begin 可以保证整个字符串都是空白时不会越界。
    std::string::size_type end = input.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(input[end - 1])))
        --end;

    // 截取 [begin, end) 区间，即去掉两端空白后的内容。
    return input.substr(begin, end - begin);
}

}

namespace m2 {

Config Config::load(const std::string& path)
{
    // 以只读方式打开文件；ifstream 默认就是读取模式。
    std::ifstream input(path.c_str());
    if (!input) {
        // 文件不存在、权限不足等情况都在这里统一报错。
        throw std::runtime_error("cannot open config file: " + path);
    }

    Config config;       // 保存本次解析出的所有配置项
    std::string line;    // 当前读取到的一行文本
    int lineNumber = 0;  // 记录行号，出错时帮助定位到具体行
    while (std::getline(input, line)) {
        ++lineNumber;
        line = trim(line);

        // 跳过空行和以 '#' 开头的整行注释。
        if (line.empty() || line[0] == '#')
            continue;

        // 找到 key 和 value 之间的分隔符 '='。
        const std::string::size_type equal = line.find('=');
        if (equal == std::string::npos)
            throw std::runtime_error("invalid config line " + std::to_string(lineNumber));

        // '=' 左边是 key，右边是 value；两端的空白都去掉。
        const std::string key = trim(line.substr(0, equal));
        const std::string value = trim(line.substr(equal + 1));
        if (key.empty())
            throw std::runtime_error("empty config key on line " + std::to_string(lineNumber));

        // 重复的 key 直接覆盖，因此后出现的配置优先生效。
        config.values_[key] = value;
    }

    return config;
}

bool Config::has(const std::string& key) const
{
    // find() 返回 end() 说明 map 中没有这个 key。
    return values_.find(key) != values_.end();
}

std::string Config::get(const std::string& key) const
{
    const std::map<std::string, std::string>::const_iterator it =
        values_.find(key);
    if (it == values_.end()) {
        // 必需配置缺失时直接报错，避免调用方拿到空字符串后继续运行。
        throw std::runtime_error("missing config key: " + key);
    }
    return it->second;
}

std::string Config::get(const std::string& key,
                        const std::string& fallback) const
{
    const std::map<std::string, std::string>::const_iterator it =
        values_.find(key);
    // key 存在就返回对应的值，不存在则返回调用方给出的默认值。
    return it == values_.end() ? fallback : it->second;
}

int Config::getInt(const std::string& key, int fallback) const
{
    // 使用带默认值的 get()：key 不存在时得到空串，从而走 fallback 分支。
    const std::string value = get(key, std::string());
    if (value.empty()) {
        return fallback;
    }

    try {
        // stoi 解析字符串开头的整数，例如 "8080" -> 8080。
        return std::stoi(value);
    } catch (const std::exception&) {
        // 没有任何数字或超出 int 范围时，转换成明确的配置错误信息。
        throw std::runtime_error("config key is not an integer: " + key);
    }
}

}
