#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pwd.h>
#include <grp.h>
#include <ctime>
#include <cerrno>
#include <cstring>
#include <cmath>

#include <iostream>
#include <vector>
#include <string>
#include <algorithm>
#include <iomanip>
#include <sstream>

struct Options {
    bool longFormat = false;
    bool reverse = false;
    bool humanReadable = false;
    std::string path = ".";
};

struct Entry {
    std::string name;
    std::string fullPath;
    struct stat st;
};

void printUsage(const char* argv0){
    std::cerr << "Usage: " << argv0 << " [-l] [-r] [-h] [path]\n";
}

bool parseCommandLineArguments(int argc, char* argv[], Options& options){
    int opt = 0;

    while ((opt = getopt(argc, argv, "lrh")) != -1) {
        switch (opt) {
        case 'l':
            options.longFormat = true;
            break;
        case 'r':
            options.reverse = true;
            break;
        case 'h':
            options.humanReadable = true;
            break;
        default:
            printUsage(argv[0]);
            return false;
        }
    }

    int remaining = argc - optind;
    if (remaining > 1) {
        std::cerr << "Error: only one path is allowed\n";
        return false;
    }

    if (remaining == 1) {
        options.path = argv[optind];
    }

    return true;
}

bool readDirectory(const std::string& path, std::vector<Entry>& entries){
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        std::cerr << "Error: cannot access '" << path << "': " << std::strerror(errno) << '\n';
        return false;
    }

    if (!S_ISDIR(st.st_mode)) {
        std::cerr << "Error: path is not a directory: '" << path << "'\n";
        return false;
    }

    DIR* dir = opendir(path.c_str());
    if (dir == NULL) {
        std::cerr << "Error: cannot open directory '" << path << "': "
                  << std::strerror(errno) << std::endl;
        return false;
    }

    errno = 0;
    struct dirent* dp = NULL;

    while ((dp = readdir(dir)) != NULL) {
        std::string name = dp->d_name;

        if (!name.empty() && name[0] == '.') {
            continue;
        }

        Entry entry;
        entry.name = name;

        if (!path.empty() && path[path.size() - 1] == '/') {
            entry.fullPath = path + name;
        } else {
            entry.fullPath = path + "/" + name;
        }

        if (lstat(entry.fullPath.c_str(), &entry.st) != 0) {
            std::cerr << "Warning: cannot stat '" << entry.fullPath << "': " << std::strerror(errno) << std::endl;
            continue;
        }

        entries.push_back(entry);
    }
    if (errno != 0) {
        std::cerr << "Error: failed while reading directory '" << path << "': "
                  << std::strerror(errno) << std::endl;
        closedir(dir);
        return false;
    }

    if (closedir(dir) != 0) {
        std::cerr << "Error: cannot close directory '" << path << "': "
                  << std::strerror(errno) << std::endl;
        return false;
    }

    return true;
}

void sortEntries(std::vector<Entry>& entries, bool reverse) {
    std::sort(entries.begin(), entries.end(),
              [reverse](const Entry& a, const Entry& b) {
                  return reverse ? a.name > b.name : a.name < b.name;
              });
}

char getFileTypeChar(mode_t mode) {
    if (S_ISREG(mode))  return '-';
    if (S_ISDIR(mode))  return 'd';
    if (S_ISLNK(mode))  return 'l';
    if (S_ISCHR(mode))  return 'c';
    if (S_ISBLK(mode))  return 'b';
    if (S_ISFIFO(mode)) return 'p';
    if (S_ISSOCK(mode)) return 's';
    return '?';
}

std::string getPermissions(mode_t mode) {
    std::string perms(10, '-');

    perms[0] = getFileTypeChar(mode);

    perms[1] = (mode & S_IRUSR) ? 'r' : '-';
    perms[2] = (mode & S_IWUSR) ? 'w' : '-';
    perms[3] = (mode & S_IXUSR) ? 'x' : '-';

    perms[4] = (mode & S_IRGRP) ? 'r' : '-';
    perms[5] = (mode & S_IWGRP) ? 'w' : '-';
    perms[6] = (mode & S_IXGRP) ? 'x' : '-';

    perms[7] = (mode & S_IROTH) ? 'r' : '-';
    perms[8] = (mode & S_IWOTH) ? 'w' : '-';
    perms[9] = (mode & S_IXOTH) ? 'x' : '-';

    if (mode & S_ISUID) perms[3] = (mode & S_IXUSR) ? 's' : 'S';
    if (mode & S_ISGID) perms[6] = (mode & S_IXGRP) ? 's' : 'S';
    if (mode & S_ISVTX) perms[9] = (mode & S_IXOTH) ? 't' : 'T';

    return perms;
}

std::string formatSize(off_t size, bool humanReadable) {
    if (!humanReadable) {
        return std::to_string(size);
    }

    const char* suffixes[] = {"B", "K", "M", "G", "T"};
    double value = static_cast<double>(size);
    int suffixIndex = 0;

    while (value >= 1024.0 && suffixIndex < 4) {
        value /= 1024.0;
        ++suffixIndex;
    }

    if (suffixIndex == 0) {
        return std::to_string(size);
    }

    std::ostringstream out;

    if (value >= 10.0) {
        out << static_cast<long long>(std::round(value));
    } else {
        out << std::fixed << std::setprecision(1) << value;
    }

    out << suffixes[suffixIndex];
    return out.str();
}

std::string formatTime(time_t t) {
    char buffer[64] = {0};
    struct tm* tmInfo = std::localtime(&t);
    if (tmInfo == NULL) {
        return "??? ?? ??:??";
    }

    std::strftime(buffer, sizeof(buffer), "%b %d %H:%M", tmInfo);
    return std::string(buffer);
}

void printLong(const std::vector<Entry>& entries, bool humanReadable) {
    std::size_t linksWidth = 0;
    std::size_t ownerWidth = 0;
    std::size_t groupWidth = 0;
    std::size_t sizeWidth = 0;

    struct PreparedRow {
        std::string perms;
        std::string links;
        std::string owner;
        std::string group;
        std::string size;
        std::string timeStr;
        std::string name;
    };

    std::vector<PreparedRow> rows;
    rows.reserve(entries.size());

    for (size_t i = 0; i < entries.size(); ++i) {
        const Entry& e = entries[i];

        PreparedRow row;
        row.perms = getPermissions(e.st.st_mode);
        row.links = std::to_string(static_cast<unsigned long long>(e.st.st_nlink));

        struct passwd* pw = getpwuid(e.st.st_uid);
        row.owner = (pw != NULL) ? pw->pw_name : std::to_string(static_cast<unsigned long long>(e.st.st_uid));

        struct group* gr = getgrgid(e.st.st_gid);
        row.group = (gr != NULL) ? gr->gr_name : std::to_string(static_cast<unsigned long long>(e.st.st_gid));

        row.size = formatSize(e.st.st_size, humanReadable);
        row.timeStr = formatTime(e.st.st_mtime);
        row.name = e.name;

        linksWidth = std::max(linksWidth, row.links.size());
        ownerWidth = std::max(ownerWidth, row.owner.size());
        groupWidth = std::max(groupWidth, row.group.size());
        sizeWidth = std::max(sizeWidth, row.size.size());

        rows.push_back(row);
    }

    for (size_t i = 0; i < rows.size(); ++i) {
        const PreparedRow& row = rows[i];

        std::cout << row.perms << ' '
                  << std::setw(static_cast<int>(linksWidth)) << row.links << ' '
                  << std::left << std::setw(static_cast<int>(ownerWidth)) << row.owner << ' '
                  << std::left << std::setw(static_cast<int>(groupWidth)) << row.group << ' '
                  << std::right << std::setw(static_cast<int>(sizeWidth)) << row.size << ' '
                  << row.timeStr << "  "
                  << row.name << '\n';
    }
}

void printShort(const std::vector<Entry>& entries) {
    for (size_t i = 0; i < entries.size(); ++i) {
        std::cout << entries[i].name << '\n';
    }
}

int main(int argc, char* argv[])
{
    Options options;
    if(!parseCommandLineArguments(argc, argv, options)){
        return 1;
    }

    std::vector<Entry> entries;
    if(!readDirectory(options.path, entries)){
        return 1;
    }

    sortEntries(entries, options.reverse);
    if (options.longFormat) {
        printLong(entries, options.humanReadable);
    } else {
        printShort(entries);
    }
    return 0;
}
