#ifdef _WIN32
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif
#include <iomanip>

bool is_directory(const std::string &path) {
    struct stat buffer{};
    if (stat(path.c_str(), &buffer) != 0) {
        return false;
    }
#ifdef _WIN32
    return (buffer.st_mode & _S_IFDIR) != 0;
#else
    return S_ISDIR(buffer.st_mode);
#endif
}

bool create_directory() {
    const std::string dir_path = obs_module_get_config_path(obs_current_module(), "");
#ifdef _WIN32
    return CreateDirectoryA(dir_path.c_str(), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
    return mkdir(dir_path.c_str(), 0700) == 0 || errno == EEXIST;
#endif
}

std::string generate_random_string_with_time(size_t num_bytes) {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto current_time = std::chrono::duration_cast<std::chrono::seconds>(now).count();

    std::random_device rd;
    std::mt19937 generator(rd() ^ current_time);
    std::uniform_int_distribution<int> distribution(0, 255);

    std::vector<unsigned char> bytes(num_bytes);
    for (auto &byte: bytes) {
        byte = static_cast<unsigned char>(distribution(generator));
    }

    std::ostringstream oss;
    for (const unsigned char byte: bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }

    return oss.str();
}

std::vector<std::string> syllables = {
    "a", "ka", "sa", "ta", "na", "ha", "ma", "ya", "ra", "wa",
    "i", "ki", "shi", "chi", "ni", "hi", "mi", "ri",
    "u", "ku", "su", "tsu", "nu", "fu", "mu", "yu", "ru",
    "e", "ke", "se", "te", "ne", "he", "me", "re",
    "o", "ko", "so", "to", "no", "ho", "mo", "yo", "ro"
};

std::string generateRandomJapaneseName(const int syllablesCount) {
    std::string name;
    for (int i = 0; i < syllablesCount; ++i) {
        name += syllables[rand() % syllables.size()];
    }
    return name;
}
