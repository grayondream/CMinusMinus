#pragma once

#include <string>
#include <vector>

// CT-07: sema→codegen hand-off for compile_time reflection results. Sema
// records per-type reflection data (keyed by the type's mangled name); codegen
// materializes them as global constant arrays + slice views. Process-level
// singleton so multiple calls for the same type share one registry.
namespace cdd {

struct FieldInfoData {
    std::string name;
    size_t offset;
    size_t size;
};

struct AttributeData {
    std::string name;
};

class ReflectionRegistry {
public:
    static ReflectionRegistry& instance();

    void registerName(const std::string& key, std::string name);
    std::string getName(const std::string& key) const;

    void registerFields(const std::string& key, std::vector<FieldInfoData> fields);
    const std::vector<FieldInfoData>* getFields(const std::string& key) const;

    void registerAttributes(const std::string& key, std::vector<AttributeData> attrs);
    const std::vector<AttributeData>* getAttributes(const std::string& key) const;

private:
    ReflectionRegistry() = default;
    struct Impl;
    Impl* d = nullptr;
};

} // namespace cdd
