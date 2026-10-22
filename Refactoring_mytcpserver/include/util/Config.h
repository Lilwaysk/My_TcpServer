#ifndef UTIL_CONFIG_H
#define UTIL_CONFIG_H

#include <map>
#include <string>

namespace m2 {

/**
 * @brief 简单的键值对配置读取器。
 *
 * 配置文件每行写一条 "key = value"：
 *   - 空行以及以 '#' 开头的行会被忽略；
 *   - key 和 value 两端的空白字符会自动去掉；
 *   - 同一个 key 出现多次时，后出现的值会覆盖前面的值。
 *
 * 示例：
 *   server.port = 8080
 *   server.threads = 4
 *
 * 配置只在 load() 时读取一次，之后查询都直接访问内存中的 map，
 * 调用方不需要再关心文件 I/O。
 */
class Config {
public:
    /**
     * @brief 从指定路径加载配置文件。
     * @param path 配置文件路径。
     * @return 加载完成的 Config 对象，可按值返回并直接使用。
     * @throw std::runtime_error 文件无法打开、某行缺少 '=' 或 key 为空时抛出。
     */
    static Config load(const std::string& path);

    /**
     * @brief 判断配置项是否存在。
     * @param key 配置项名称。
     * @return 存在返回 true，否则返回 false。
     */
    bool has(const std::string& key) const;

    /**
     * @brief 获取配置项的值。
     * @param key 配置项名称。
     * @return 对应的字符串值。
     * @throw std::runtime_error key 不存在时抛出。
     */
    std::string get(const std::string& key) const;

    /**
     * @brief 获取配置项的值，key 不存在时返回默认值。
     * @param key 配置项名称。
     * @param fallback key 不存在时使用的默认值。
     */
    std::string get(const std::string& key, const std::string& fallback) const;

    /**
     * @brief 获取整数类型的配置项。
     * @param key 配置项名称。
     * @param fallback key 不存在或值为空时返回的默认值。
     * @return 转换后的整数。
     * @throw std::runtime_error 值无法转换为整数时抛出。
     */
    int getInt(const std::string& key, int fallback) const;

private:

    // key -> value 的映射；加载完成后只读，不再修改。
    std::map<std::string, std::string> values_;

};

}

#endif
