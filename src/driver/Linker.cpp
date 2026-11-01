#include "Linker.h"
#include <cstdlib>
#include <sstream>

namespace {

bool needsShellQuote(const std::string& s) {
    if (s.empty()) return true;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\'' || c == '"' ||
            c == '\\' || c == '$' || c == '`' || c == '&' || c == '|' ||
            c == ';' || c == '<' || c == '>' || c == '(' || c == ')' ||
            c == '*' || c == '?' || c == '[' || c == ']' || c == '{' ||
            c == '}' || c == '!' || c == '#') {
            return true;
        }
    }
    return false;
}

std::string shellQuote(const std::string& s) {
    if (!needsShellQuote(s)) return s;
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    out += "'";
    return out;
}

} // namespace

Linker::Linker() {
    systemLinkerPath = findSystemLinker();
}

std::string Linker::findSystemLinker() {
    const char* linkers[] = {"cc", "gcc", "ld", "ld.lld", "ld.gold", nullptr};
    for (const char** l = linkers; *l; l++) {
        std::string cmd = std::string("which ") + *l + " >/dev/null 2>&1";
        if (system(cmd.c_str()) == 0) {
            return *l;
        }
    }
    return "ld";
}

std::string Linker::constructLinkCommand(const std::vector<std::string>& objects,
                                         const std::string& output,
                                         const std::vector<std::string>& libs,
                                         const std::vector<std::string>& libPaths) {
    std::ostringstream cmd;
    cmd << shellQuote(systemLinkerPath) << " -o " << shellQuote(output);
    for (const auto& obj : objects) {
        cmd << " " << shellQuote(obj);
    }
    for (const auto& path : libPaths) {
        cmd << " -L" << shellQuote(path);
    }
    for (const auto& lib : libs) {
        cmd << " -l" << shellQuote(lib);
    }
    return cmd.str();
}

int Linker::link(const std::vector<std::string>& objects,
                 const std::string& output,
                 const std::vector<std::string>& libs,
                 const std::vector<std::string>& libPaths) {
    std::string cmd = constructLinkCommand(objects, output, libs, libPaths);
    return system(cmd.c_str());
}
