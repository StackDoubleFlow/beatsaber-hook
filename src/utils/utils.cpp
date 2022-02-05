// thx https://github.com/jbro129/Unity-Substrate-Hook-Android
#include "../../shared/utils/utils.h"
#include <sys/types.h>
#include <sys/stat.h>

#include <unistd.h>
#include <dlfcn.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <codecvt>
#include <locale>
#include "il2cpp-object-internals.h"
#include "modloader/shared/modloader.hpp"
#include "shared/utils/gc-alloc.hpp"

void safeAbort(const char* func, const char* file, int line) {
    static auto logger = Logger::get().WithContext("CRASH_UNLESS");
    // we REALLY want this to appear at least once in the log (for fastest fixing)
    for (int i = 0; i < 2; i++) {
        usleep(100000L);  // 0.1s
        // TODO: Make this eventually have a passed in context
        logger.critical("Aborting in %s at %s:%i", func, file, line);
    }
    Logger::closeAll();
    usleep(100000L);  // 0.1s
    std::terminate();  // cleans things up and then calls abort
}

__attribute__((format(printf, 4, 5))) void safeAbortMsg(const char* func, const char* file, int line, const char* fmt, ...) {
    static auto logger = Logger::get().WithContext("CRASH_UNLESS");
    // we REALLY want this to appear at least once in the log (for fastest fixing)
    for (int i = 0; i < 2; i++) {
        usleep(100000L);  // 0.1s
        // TODO: Make this eventually have a passed in context
        logger.critical("Aborting in %s at %s:%i", func, file, line);
        va_list lst;
        va_start(lst, fmt);
        logger.log_v(Logging::CRITICAL, fmt, lst);
        va_end(lst);
    }
    Logger::closeAll();
    usleep(100000L);  // 0.1s
    std::terminate();  // cleans things up and then calls abort
}

void resetSS(std::stringstream& ss) {
    ss.str("");
    ss.clear();  // Clear state flags.
}

void tabs(std::ostream& os, int tabs, int spacesPerTab) {
    for (int i = 0; i < tabs * spacesPerTab; i++) {
        os << " ";
    }
}

void print(std::stringstream& ss, Logging::Level lvl) {
    ss << std::endl;
    Logger::get().log(lvl, "%s", ss.str().c_str());
    resetSS(ss);
}

std::string string_vformat(const std::string_view format, va_list args) {
    size_t size = vsnprintf(nullptr, 0, format.data(), args) + 1; // Extra space for '\0'
    if (size <= 0)
        return "";
    std::unique_ptr<char[]> buf(new char[size]); 
    vsnprintf(buf.get(), size, format.data(), args);
    return std::string(buf.get(), buf.get() + size - 1);
}

static std::unordered_set<const void*> analyzed;
void analyzeBytes(std::stringstream& ss, const void* ptr, int indent) {
    if (!ptr || ((const uintptr_t)ptr) > 0x7fffffffffll) return;

    tabs(ss, indent);
    if (analyzed.count(ptr)) {
        ss << "! loop at 0x" << std::hex << ptr << "!";
        print(ss);
        return;
    }
    analyzed.insert(ptr);

    auto asUInts = reinterpret_cast<const uintptr_t*>(ptr);
    auto asInts = reinterpret_cast<const intptr_t*>(ptr);
    auto asChars = reinterpret_cast<const char*>(ptr);
    if (asUInts[0] >= 0x1000000000000ll && isprint(asChars[0])) {
        ss << "chars: \"" << asChars << "\"";
        ss << " (first 8 bytes in hex = 0x" << std::hex << std::setw(16) << asUInts[0] << ")";
        print(ss);
        return;
    }
    for (int i = 0; i < 4; i++) {
        if (i != 0) tabs(ss, indent);

        ss << "pos " << std::dec << i << ": 0x" << std::hex << std::setw(16) << asUInts[i];
        if (asUInts[i] >= 0x8000000000ll) {
            // todo: read no more than 8 chars or move asInts to last aligned point in string
            ss << " (as chars = \"" << reinterpret_cast<const char*>(asUInts + i) << "\")";
            ss << " (as int = " << std::dec << asInts[i] << ")";  // signed int
        } else if (asUInts[i] <= 0x7f00000000ll) {
            ss << " (as int = " << std::dec << asUInts[i] << ")";
        }
        else {
            Dl_info inf;
            if (dladdr((void*)asUInts[i], &inf)) {
                ss << " (dli_fname: " << inf.dli_fname << ", dli_fbase: " << std::hex << std::setw(16) << (uintptr_t)inf.dli_fbase;
                ss << ", offset = 0x" << std::hex << std::setw(8) << (asUInts[i] - (uintptr_t)inf.dli_fbase);
                if (inf.dli_sname) {
                    ss << ", dli_sname: " << inf.dli_sname << ", dli_saddr: " << std::hex << std::setw(16) << (uintptr_t)inf.dli_saddr;
                }
                ss << ")";
            }
        }
        print(ss);
        if (asUInts[i] > 0x7f00000000ll) {
            analyzeBytes(ss, (void*)asUInts[i], indent + 1);
        }
    }
}

static uintptr_t soSize = 0;

uintptr_t getLibil2cppSize() {
    static auto contextLogger = Logger::get().WithContext("getSize");
    if (soSize == 0) {
        struct stat st;
        if (!stat(Modloader::getLibIl2CppPath().c_str(), &st)) {
            soSize = st.st_size;
        }
        contextLogger.debug("libil2cpp.so size: 0x%lx", soSize);
    }
    return soSize;
}

std::string string_format(const char* format, ...) {
    va_list lst;
    va_start(lst, format);
    auto s = string_vformat(format, lst);
    va_end(lst);
    return s;
}

void analyzeBytes(const void* ptr) {
    analyzed.clear();
    std::stringstream ss;
    ss << std::setfill('0');
    ss << "ptr: " << std::hex << std::setw(16) << (uintptr_t) ptr;
    print(ss);
    analyzeBytes(ss, ptr, 0);
}

uintptr_t baseAddr(const char *soname)  // credits to https://github.com/ikoz/AndroidSubstrate_hookingC_examples/blob/master/nativeHook3/jni/nativeHook3.cy.cpp
{
    void *imagehandle = dlopen(soname, RTLD_LOCAL | RTLD_LAZY);
    if (soname == NULL)
        return (uintptr_t)NULL;
    if (imagehandle == NULL)
        return (uintptr_t)NULL;

    FILE *f = NULL;
    char line[200] = {0};
    char *state = NULL;
    char *tok = NULL;
    char * baseAddr = NULL;
    if ((f = fopen("/proc/self/maps", "r")) == NULL)
        return (uintptr_t)NULL;
    while (fgets(line, 199, f) != NULL)
    {
        tok = strtok_r(line, "-", &state);
        baseAddr = tok;
        strtok_r(NULL, "\t ", &state);
        strtok_r(NULL, "\t ", &state); // "r-xp" field
        strtok_r(NULL, "\t ", &state); // "0000000" field
        strtok_r(NULL, "\t ", &state); // "01:02" field
        strtok_r(NULL, "\t ", &state); // "133224" field
        tok = strtok_r(NULL, "\t ", &state); // path field

        if (tok != NULL) {
            int i;
            for (i = (int)strlen(tok)-1; i >= 0; --i) {
                if (!(tok[i] == ' ' || tok[i] == '\r' || tok[i] == '\n' || tok[i] == '\t'))
                    break;
                tok[i] = 0;
            }
            {
                size_t toklen = strlen(tok);
                size_t solen = strlen(soname);
                if (toklen > 0) {
                    if (toklen >= solen && strcmp(tok + (toklen - solen), soname) == 0) {
                        fclose(f);
                        return (uintptr_t)strtoll(baseAddr,NULL,16);
                    }
                }
            }
        }
    }
    fclose(f);
    return (uintptr_t)NULL;
}

uintptr_t location; // save lib.so base address so we do not have to recalculate every time causing lag.

uintptr_t getRealOffset(const void* offset) // calculate dump.cs address + lib.so base address.
{
    if (location == 0)
    {
        //arm
        // TOOD: Lets get the instance via some sort of initialization function
        // OR we make EVERYTHING on an instance level
        location = baseAddr(Modloader::getLibIl2CppPath().c_str());
    }
    return location + (uintptr_t)offset;
}

int mkpath(std::string_view file_path) {
    return system((std::string("mkdir -p -m +rw ") + file_path.data()).c_str());
}

uintptr_t findPattern(uintptr_t dwAddress, const char* pattern, uintptr_t dwSearchRangeLen) {
    #define in_range(x, a, b) (x >= a && x <= b)
    #define get_bits(x) (in_range((x & (~0x20)), 'A', 'F') ? ((x & (~0x20)) - 'A' + 0xA): (in_range(x, '0', '9') ? x - '0': 0))
    #define get_byte(x) (get_bits(x[0]) << 4 | get_bits(x[1]))

    // To avoid a lot of bad match candidates, pre-process wildcards at the front of the pattern
    uintptr_t skippedStartBytes = 0;
    while(pattern[0] == '\?') {
        // see comments below for insight on these numbers
        pattern += (pattern[1] == '\?') ? 3 : 2;
        skippedStartBytes++;
    }
    uintptr_t match = 0;  // current match candidate
    uintptr_t len = strlen(CRASH_UNLESS(pattern));
    if (dwSearchRangeLen < len) {
        return 0;
    }
    const char* pat = pattern;  // current spot in the pattern

    for (uintptr_t pCur = dwAddress + skippedStartBytes; pCur < dwAddress + dwSearchRangeLen; pCur++) {
        // If pat[0] is null char, we are done, or if pat >= pattern + len
        if (pat >= pattern + len || !pat[0] || !pat[1]) {
            return match;
        }
        // For each byte, if the pattern starts with a ? or the current byte matches:
        if (pat[0] == '\?' || *(char *)pCur == get_byte(pat)) {
            // If we do not have a match, begin it
            if (!match) {
                match = pCur - skippedStartBytes;
            }
            // If our next character is at the end of our pattern, we have a match
            if (pat + 1 >= pattern + len) {
                return match;
            }
            if (pat[0] != '\?' || pat[1] == '\?') {
                pat += 3;  // advance past "xy " or "?? "
            } else {
                pat += 2;  // advance past "? "
            }
        }
        else {
            // reset search position to beginning of the failed match; for loop will begin new search at match + 1
            if (match) pCur = match + skippedStartBytes;
            pat = pattern;
            match = 0;
        }
    }
    return 0;
}

uintptr_t findUniquePattern(bool& multiple, uintptr_t dwAddress, const char* pattern, const char* label, uintptr_t dwSearchRangeLen) {
    uintptr_t firstMatchAddr = 0, newMatchAddr, start = dwAddress, dwEnd = dwAddress + dwSearchRangeLen;
    int matches = 0;
    Logger::get().debug("Sigscan for pattern: %s", pattern);
    while (start > 0 && start < dwEnd && (newMatchAddr = findPattern(start, pattern, dwEnd - start))) {
        if (!firstMatchAddr) firstMatchAddr = newMatchAddr;
        matches++;
        if (label) Logger::get().debug("Sigscan found possible \"%s\": offset 0x%lX, pointer 0x%lX", label, newMatchAddr - dwAddress, newMatchAddr);
        start = newMatchAddr + 1;
        Logger::get().debug("start = 0x%lX, end = 0x%lX", start, dwEnd);
        usleep(1000);
    }
    if (matches > 1) {
        multiple = true;
        Logger::get().warning("Multiple sig scan matches for \"%s\"!", label);
    }
    return firstMatchAddr;
}

uintptr_t findUniquePatternInLibil2cpp(bool& multiple, const char* pattern, const char* label) {
    // Essentially call findUniquePattern for each segment listed in /proc/self/maps
    std::ifstream procMap("/proc/self/maps");
    std::string line;
    uintptr_t match = 0;
    while (std::getline(procMap, line)) {
        if (line.find("libil2cpp.so") == std::string::npos) {
            continue;
        }
        auto idx = line.find_first_of('-');
        if (idx == std::string::npos) {
            CRASH_UNLESS(false);
        }
        auto startAddr = std::stoul(line.substr(0, idx), nullptr, 16);
        auto spaceIdx = line.find_first_of(' ');
        if (spaceIdx == std::string::npos) {
            CRASH_UNLESS(false);
        }
        auto endAddr = std::stoul(line.substr(idx + 1, spaceIdx - idx - 1), nullptr, 16);
        // Permissions are 4 characters
        auto perms = line.substr(spaceIdx + 1, 4);
        if (perms.find('r') != std::string::npos) {
            // Search between start and end
            match = findUniquePattern(multiple, startAddr, pattern, label, endAddr - startAddr);
        }
    }
    procMap.close();
    return match;
}

// C# SPECIFIC

void setcsstr(Il2CppString* in, std::u16string_view str) {
    in->length = str.length();
    for(int i = 0; i < in->length; i++) {
        // Can assume that each char is only a single char (a single word --> double word)
        in->chars[i] = str[i];
    }
    in->chars[in->length] = (Il2CppChar)'\u0000';
}

std::string to_utf8(std::u16string_view view) {
    return std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.to_bytes(view.data());
}

std::u16string to_utf16(std::string_view view) {
    return std::wstring_convert<std::codecvt_utf8_utf16<char16_t>, char16_t>{}.from_bytes(view.data());
}

std::u16string_view csstrtostr(Il2CppString* in)
{
    return {in->chars, static_cast<uint32_t>(in->length)};
}

// Thanks DaNike!
void dump(int before, int after, void* ptr) {
    Logger::get().debug("Dumping Immediate Pointer: %p: %08x", ptr, *reinterpret_cast<int*>(ptr));
    auto begin = static_cast<int*>(ptr) - before;
    auto end = static_cast<int*>(ptr) + after;
    for (auto cur = begin; cur != end; ++cur) {
        Logger::get().debug("%p: %08x", cur, *cur);
    }
}

// SETTINGS

bool fileexists(std::string_view filename) {
    return access(filename.data(), W_OK | R_OK) != -1;
}

bool direxists(std::string_view dirname) {
    struct stat info;

    if (stat(dirname.data(), &info) != 0) {
        return false;
    } if (info.st_mode & S_IFDIR) {
        return true;
    }
    return false;
}

std::string readfile(std::string_view filename) {
    std::ifstream t(filename.data());
    if (!t.is_open()) {
        return "";
    }
    std::stringstream buffer;
    buffer << t.rdbuf();
    return buffer.str();
}

std::vector<char> readbytes(std::string_view filename) {
    std::ifstream infile(filename.data(), std::ios_base::binary);
    if (!infile.is_open()) {
        return std::vector<char>();
    }
    return std::vector<char>(std::istreambuf_iterator<char>(infile), std::istreambuf_iterator<char>());
}

bool writefile(std::string_view filename, std::string_view text) {
    std::ofstream t(filename.data());
    if (t.is_open()) {
        t << text;
        return true;
    }
    return false;
}

bool deletefile(std::string_view filename) {
    if (fileexists(filename))
        return remove(filename.data()) == 0;
    return false;
}
