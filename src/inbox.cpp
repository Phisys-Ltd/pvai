#include "inbox.h"

#include "common.h"
#include "text.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <ctime>
#include <filesystem>

namespace pvai {

namespace fs = std::filesystem;

std::string inboxRoot()
{
    // Created on demand and canonical (symlinks in HOME resolved), so that every
    // path derived from it agrees with what realpath() gives back later.
    const std::string path = homeDir() + "/.cache/pvai/inbox";
    std::error_code ec;
    fs::create_directories(path, ec);
    ::chmod(path.c_str(), 0700);
    char real[PATH_MAX];
    return ::realpath(path.c_str(), real) ? std::string(real) : path;
}

static bool goodId(const std::string &id)
{
    return !id.empty() && id != "." && id != ".." && id.size() <= 200 && id.find('/') == std::string::npos
        && std::none_of(id.begin(), id.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
}

std::string inboxDir(const std::string &session)
{
    return goodId(session) ? inboxRoot() + "/" + session : std::string();
}

std::string sanitizeUploadName(const std::string &name)
{
    // The basename: what follows the last / (or \).
    std::string base = name.substr(name.find_last_of("/\\") == std::string::npos ? 0 : name.find_last_of("/\\") + 1);
    std::string clean;
    for (const char c : base)
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f)
            clean += c;
    for (std::size_t at; (at = clean.find("..")) != std::string::npos;)
        clean.erase(at, 1);
    clean = trim(clean, " ");
    clean.erase(0, clean.find_first_not_of('.') == std::string::npos ? clean.size() : clean.find_first_not_of('.'));
    clean = trim(clean, " ");
    if (clean.size() > kMaxUploadName) {
        const auto dot = clean.rfind('.');
        const std::string extension = dot != std::string::npos && dot > 0 && clean.size() - dot <= 20 ? clean.substr(dot) : "";
        const std::size_t room = kMaxUploadName - extension.size();
        clean = clean.substr(0, utf8Boundary(clean, room)) + extension;
    }
    return clean;
}

std::string inboxFile(const std::string &session, const std::string &path)
{
    const std::string dir = inboxDir(session);
    if (dir.empty() || path.empty() || path[0] != '/' || path.find('\0') != std::string::npos)
        return {};
    char real[PATH_MAX], root[PATH_MAX];
    if (!::realpath(dir.c_str(), root) || !::realpath(path.c_str(), real))
        return {};
    const std::string base = std::string(root) + "/";
    if (std::string(real).compare(0, base.size(), base) != 0)
        return {};
    struct stat st;
    if (::stat(real, &st) != 0 || !S_ISREG(st.st_mode))
        return {};
    return real;
}

void removeInbox(const std::string &session)
{
    const std::string dir = inboxDir(session);
    if (dir.empty())
        return;
    std::error_code ec;
    fs::remove_all(dir, ec);
}

void cleanInbox(const std::set<std::string> *live, std::chrono::seconds maxAge)
{
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(inboxRoot(), ec), end; !ec && it != end; it.increment(ec))
        if (it->symlink_status(ec).type() == fs::file_type::directory)
            dirs.push_back(it->path());
    for (const auto &dir : dirs) {
        if (live && !live->count(dir.filename().string())) {
            fs::remove_all(dir, ec);
            continue;
        }
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const auto written = fs::last_write_time(it->path(), ec);
            if (!ec && now - written >= maxAge)
                fs::remove_all(it->path(), ec);
            ec.clear();
        }
        ec.clear();
        if (fs::is_empty(dir, ec) && !ec)
            fs::remove(dir, ec);
    }
}

namespace {

void makePrivateDir(const std::string &path)
{
    std::error_code ec;
    fs::create_directories(path, ec);
    ::chmod(path.c_str(), 0700);
}

unsigned long long dirUsage(const std::string &dir, const std::string &except)
{
    unsigned long long total = 0;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->path().string() == except || !it->is_regular_file(e2))
            continue;
        total += it->file_size(e2);
    }
    return total;
}

std::string stamp()
{
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    ::localtime_r(&now, &local);
    char text[32];
    std::strftime(text, sizeof text, "%Y%m%d-%H%M%S-", &local);
    return text;
}

} // namespace

static std::string hexOf(const unsigned char *digest, unsigned int length)
{
    static const char hex[] = "0123456789abcdef";
    std::string sum;
    for (unsigned int i = 0; i < length; ++i) {
        sum += hex[digest[i] >> 4];
        sum += hex[digest[i] & 15];
    }
    return sum;
}

std::string fileSha256(const std::string &path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return {};
    EVP_MD_CTX *md = EVP_MD_CTX_new();
    EVP_DigestInit_ex(md, EVP_sha256(), nullptr);
    char chunk[65536];
    for (ssize_t n; (n = ::read(fd, chunk, sizeof chunk)) > 0;)
        EVP_DigestUpdate(md, chunk, static_cast<std::size_t>(n));
    ::close(fd);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_DigestFinal_ex(md, digest, &length);
    EVP_MD_CTX_free(md);
    return hexOf(digest, length);
}

InboxUpload::InboxUpload(std::string session, std::string name, unsigned long long size, std::string sha256)
    : session_(std::move(session))
    , name_(std::move(name))
    , sha_(std::move(sha256))
    , size_(size)
{
}

InboxUpload::~InboxUpload()
{
    abort();
    if (md_)
        EVP_MD_CTX_free(md_);
}

std::string InboxUpload::open()
{
    const std::string clean = sanitizeUploadName(name_);
    dir_ = inboxDir(session_);
    if (clean.empty() || dir_.empty())
        return "bad-name";
    makePrivateDir(inboxRoot());
    makePrivateDir(dir_);
    if (dirUsage(dir_, "") + size_ > kMaxUploadSession)
        return "quota";
    // NAME.part first; a counter before the extension when the name is taken.
    const auto dot = clean.rfind('.');
    const std::string stem = dot != std::string::npos && dot > 0 ? clean.substr(0, dot) : clean;
    const std::string extension = stem.size() == clean.size() ? "" : clean.substr(stem.size());
    for (int n = 1; n < 1000 && fd_ < 0; ++n) {
        stored_ = stamp() + stem + (n > 1 ? "-" + std::to_string(n) : "") + extension;
        path_ = dir_ + "/" + stored_;
        part_ = path_ + ".part";
        struct stat st;
        if (::lstat(path_.c_str(), &st) == 0)
            continue;
        fd_ = ::open(part_.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    }
    if (fd_ < 0)
        return "write-failed";
    md_ = EVP_MD_CTX_new();
    if (!md_ || EVP_DigestInit_ex(md_, EVP_sha256(), nullptr) != 1) {
        abort();
        return "write-failed";
    }
    return {};
}

bool InboxUpload::write(const char *data, std::size_t size)
{
    if (fd_ < 0 || !writeAll(fd_, data, size))
        return false;
    EVP_DigestUpdate(md_, data, size);
    received_ += size;
    return true;
}

std::string InboxUpload::finish()
{
    if (fd_ < 0 || received_ != size_)
        return "short";
    ::close(fd_);
    fd_ = -1;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    EVP_DigestFinal_ex(md_, digest, &length);
    if (hexOf(digest, length) != sha_) {
        abort();
        return "checksum";
    }
    // Other uploads of the session may have finished meanwhile.
    if (dirUsage(dir_, part_) + size_ > kMaxUploadSession) {
        abort();
        return "quota";
    }
    // No clobbering: the final name appears by a link, the .part goes after.
    if (::link(part_.c_str(), path_.c_str()) == 0) {
        ::unlink(part_.c_str());
    } else if (errno == EEXIST || ::rename(part_.c_str(), path_.c_str()) != 0) {
        abort();
        return "write-failed";
    }
    part_.clear();
    return {};
}

void InboxUpload::abort()
{
    if (fd_ >= 0)
        ::close(fd_);
    fd_ = -1;
    if (!part_.empty())
        ::unlink(part_.c_str());
    part_.clear();
}

} // namespace pvai
