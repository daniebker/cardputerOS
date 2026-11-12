// Persistent storage: small settings in NVS, files on the internal LittleFS partition.
#pragma once
#include <string>
#include <vector>

namespace storage {

void begin();       // settings (NVS) only: fast
void mountFiles();  // LittleFS: slow on the very first boot
bool mounted();

// Files (paths are relative to the LittleFS root, e.g. "/ssh/id_ed25519").
bool readFile(const char* path, std::string& out);
bool writeFile(const char* path, const std::string& data);
bool exists(const char* path);
bool remove(const char* path);
bool makeDir(const char* path);
// Absolute VFS path for C stdio / libraries, e.g. "/littlefs/ssh/id_ed25519".
std::string vfsPath(const char* path);

std::vector<std::string> splitLines(const std::string& text);
std::vector<std::string> splitFields(const std::string& line, char sep = '\t');

// Settings (NVS key/value). Keys are limited to 15 characters.
int getInt(const char* key, int def);
void putInt(const char* key, int value);
std::string getStr(const char* key, const char* def = "");
void putStr(const char* key, const std::string& value);

}  // namespace storage
