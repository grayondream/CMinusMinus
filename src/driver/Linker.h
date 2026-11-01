#pragma once
#include <string>
#include <vector>

class Linker {
    std::string systemLinkerPath;
public:
    Linker();
    std::string findSystemLinker();
    // -l/-L are honored; every path/name is shell-quoted. Library order is
    // preserved (objects first, then user libraries) so archive/override
    // semantics stay predictable.
    std::string constructLinkCommand(const std::vector<std::string>& objects,
                                     const std::string& output,
                                     const std::vector<std::string>& libs = {},
                                     const std::vector<std::string>& libPaths = {});
    int link(const std::vector<std::string>& objects,
             const std::string& output,
             const std::vector<std::string>& libs = {},
             const std::vector<std::string>& libPaths = {});
};
