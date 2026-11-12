#include "sema/ReflectionRegistry.h"

#include <unordered_map>

namespace cdd {

struct ReflectionRegistry::Impl {
    std::unordered_map<std::string, std::string> names;
    std::unordered_map<std::string, std::vector<FieldInfoData>> fields;
    std::unordered_map<std::string, std::vector<AttributeData>> attributes;
};

ReflectionRegistry& ReflectionRegistry::instance() {
    static ReflectionRegistry registry;
    if (!registry.d) registry.d = new Impl();
    return registry;
}

void ReflectionRegistry::registerName(const std::string& key, std::string name) {
    instance().d->names[key] = std::move(name);
}

std::string ReflectionRegistry::getName(const std::string& key) const {
    auto it = d->names.find(key);
    return it != d->names.end() ? it->second : std::string();
}

void ReflectionRegistry::registerFields(const std::string& key, std::vector<FieldInfoData> fields) {
    instance().d->fields[key] = std::move(fields);
}

const std::vector<FieldInfoData>* ReflectionRegistry::getFields(const std::string& key) const {
    auto it = d->fields.find(key);
    return it != d->fields.end() ? &it->second : nullptr;
}

void ReflectionRegistry::registerAttributes(const std::string& key, std::vector<AttributeData> attrs) {
    instance().d->attributes[key] = std::move(attrs);
}

const std::vector<AttributeData>* ReflectionRegistry::getAttributes(const std::string& key) const {
    auto it = d->attributes.find(key);
    return it != d->attributes.end() ? &it->second : nullptr;
}

} // namespace cdd
