#include "sftp_server.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// sftp.h declares its server functions only for a server build.
#ifndef WITH_SERVER
#define WITH_SERVER
#endif
#include <libssh/sftp.h>

#include "file_export.hpp"
#include "sftp_paths.hpp"

namespace ql
{

    // The most a single READ is answered with, whatever the client asks
    // for. OpenSSH asks for 32 KB at a time.
    static const std::uint32_t kMaxReadBytes = 64 * 1024;

    // How many names one READDIR reply carries.
    static const std::size_t kNamesPerReply = 64;

    // One file or folder, as a listing or STAT describes it.
    struct SftpEntry
    {
        std::string name;
        std::uint32_t permissions = 0;
        std::uint64_t size = 0;
        std::int64_t mtime = 0;
    };

    // What an SFTP handle stands for: an open file, or a folder being
    // listed.
    struct SftpHandle
    {
        bool is_directory = false;

        // A folder: its entries, read when it was opened, and how many have
        // been sent.
        std::vector<SftpEntry> entries;
        std::size_t next_entry = 0;

        // A file.
        int fd = -1;
        bool writing = false;
        // An upload: where it's being written, where it goes once closed,
        // how large it may grow (SftpUploadLimit), and whether it went over
        // that or failed to write.
        std::string temp_path;
        std::string final_path;
        std::uint64_t limit = 0;
        std::uint64_t size = 0;
        bool failed = false;
    };

    static void FillAttributes(const SftpEntry& entry, sftp_attributes_struct* attributes)
    {
        attributes->flags = SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS | SSH_FILEXFER_ATTR_ACMODTIME;
        attributes->size = entry.size;
        attributes->permissions = entry.permissions;
        attributes->atime = static_cast<std::uint32_t>(entry.mtime);
        attributes->mtime = static_cast<std::uint32_t>(entry.mtime);
    }

    class SftpSession
    {
    public:
        SftpSession(sftp_session sftp, ssh_channel channel, const std::string& db_path, const std::string& username,
                    bool view_only)
            : sftp_(sftp),
              channel_(channel),
              exports_dir_(SessionExportsDir(db_path, username)),
              imports_dir_(SessionImportsDir(db_path, username)),
              username_(username),
              view_only_(view_only)
        {
        }

        ~SftpSession()
        {
            for (const std::unique_ptr<SftpHandle>& handle : handles_)
            {
                CloseFile(handle.get());
            }
        }

        // Answers one request. Every request gets exactly one reply.
        void Handle(sftp_client_message message)
        {
            switch (sftp_client_message_get_type(message))
            {
                case SSH_FXP_REALPATH:
                    RealPath(message);
                    break;
                case SSH_FXP_STAT:
                case SSH_FXP_LSTAT:
                    Stat(message);
                    break;
                case SSH_FXP_FSTAT:
                    FileStat(message);
                    break;
                case SSH_FXP_OPENDIR:
                    OpenDirectory(message);
                    break;
                case SSH_FXP_READDIR:
                    ReadDirectory(message);
                    break;
                case SSH_FXP_OPEN:
                    Open(message);
                    break;
                case SSH_FXP_READ:
                    Read(message);
                    break;
                case SSH_FXP_WRITE:
                    Write(message);
                    break;
                case SSH_FXP_CLOSE:
                    Close(message);
                    break;
                case SSH_FXP_REMOVE:
                    Remove(message);
                    break;
                case SSH_FXP_SETSTAT:
                case SSH_FXP_FSETSTAT:
                    SetStat(message);
                    break;
                default:
                    sftp_reply_status(message, SSH_FX_OP_UNSUPPORTED, "Not supported");
                    break;
            }
        }

    private:
        bool AreaWritable(SftpArea area) const
        {
            return area == SftpArea::kImports && !view_only_;
        }

        std::string AreaDir(SftpArea area) const
        {
            return area == SftpArea::kExports ? exports_dir_ : imports_dir_;
        }

        std::string RealPathOf(const SftpPath& path) const
        {
            return AreaDir(path.area) + "/" + path.name;
        }

        SftpEntry FolderEntry(const std::string& name, SftpArea area) const
        {
            SftpEntry entry;
            entry.name = name;
            entry.permissions = 0040000 | (area == SftpArea::kRoot || !AreaWritable(area) ? 0555u : 0755u);
            // A folder nothing has been put in yet doesn't exist on disk.
            entry.mtime = static_cast<std::int64_t>(std::time(nullptr));
            if (area != SftpArea::kRoot)
            {
                struct stat info{};
                if (::stat(AreaDir(area).c_str(), &info) == 0)
                {
                    entry.mtime = static_cast<std::int64_t>(info.st_mtime);
                }
            }
            return entry;
        }

        SftpEntry FileEntry(const std::string& name, SftpArea area, const struct stat& info) const
        {
            SftpEntry entry;
            entry.name = name;
            entry.permissions = 0100000 | (AreaWritable(area) ? 0644u : 0444u);
            entry.size = static_cast<std::uint64_t>(info.st_size);
            entry.mtime = static_cast<std::int64_t>(info.st_mtime);
            return entry;
        }

        // `path`'s entry, if it exists. Only regular files count: a link or
        // anything else someone put in the folder is invisible.
        bool Lookup(const SftpPath& path, SftpEntry* entry) const
        {
            if (path.name.empty())
            {
                *entry = FolderEntry(SftpPathString(path), path.area);
                return true;
            }
            struct stat info{};
            if (::lstat(RealPathOf(path).c_str(), &info) != 0 || !S_ISREG(info.st_mode))
            {
                return false;
            }
            *entry = FileEntry(path.name, path.area, info);
            return true;
        }

        bool ResolveMessagePath(sftp_client_message message, SftpPath* path) const
        {
            const char* filename = sftp_client_message_get_filename(message);
            if (filename == nullptr || !ResolveSftpPath(filename, path))
            {
                sftp_reply_status(message, SSH_FX_NO_SUCH_FILE, "No such file");
                return false;
            }
            return true;
        }

        SftpHandle* FindHandle(sftp_client_message message) const
        {
            SftpHandle* handle = static_cast<SftpHandle*>(sftp_handle(sftp_, message->handle));
            if (handle == nullptr)
            {
                sftp_reply_status(message, SSH_FX_INVALID_HANDLE, "Invalid handle");
            }
            return handle;
        }

        void ReplyHandle(sftp_client_message message, std::unique_ptr<SftpHandle> handle)
        {
            ssh_string id = sftp_handle_alloc(sftp_, handle.get());
            if (id == nullptr)
            {
                CloseFile(handle.get());
                sftp_reply_status(message, SSH_FX_FAILURE, "Too many open files");
                return;
            }
            handles_.emplace_back(std::move(handle));
            sftp_reply_handle(message, id);
            ssh_string_free(id);
        }

        void RealPath(sftp_client_message message) const
        {
            SftpPath path;
            if (!ResolveMessagePath(message, &path))
            {
                return;
            }
            std::string resolved = SftpPathString(path);
            sftp_attributes_struct attributes{};
            sftp_reply_name(message, resolved.c_str(), &attributes);
        }

        void Stat(sftp_client_message message) const
        {
            SftpPath path;
            if (!ResolveMessagePath(message, &path))
            {
                return;
            }
            SftpEntry entry;
            if (!Lookup(path, &entry))
            {
                sftp_reply_status(message, SSH_FX_NO_SUCH_FILE, "No such file");
                return;
            }
            sftp_attributes_struct attributes{};
            FillAttributes(entry, &attributes);
            sftp_reply_attr(message, &attributes);
        }

        void FileStat(sftp_client_message message) const
        {
            SftpHandle* handle = FindHandle(message);
            if (handle == nullptr)
            {
                return;
            }
            SftpEntry entry;
            if (handle->is_directory)
            {
                entry.permissions = 0040555;
            }
            else
            {
                struct stat info{};
                if (::fstat(handle->fd, &info) != 0)
                {
                    sftp_reply_status(message, SSH_FX_FAILURE, "Can't read the file");
                    return;
                }
                entry.permissions = 0100000 | (handle->writing ? 0644u : 0444u);
                entry.size = static_cast<std::uint64_t>(info.st_size);
                entry.mtime = static_cast<std::int64_t>(info.st_mtime);
            }
            sftp_attributes_struct attributes{};
            FillAttributes(entry, &attributes);
            sftp_reply_attr(message, &attributes);
        }

        void OpenDirectory(sftp_client_message message)
        {
            SftpPath path;
            if (!ResolveMessagePath(message, &path))
            {
                return;
            }
            if (!path.name.empty())
            {
                sftp_reply_status(message, SSH_FX_NO_SUCH_FILE, "Not a folder");
                return;
            }

            std::unique_ptr<SftpHandle> handle = std::make_unique<SftpHandle>();
            handle->is_directory = true;
            if (path.area == SftpArea::kRoot)
            {
                handle->entries.reserve(2);
                handle->entries.emplace_back(FolderEntry("exports", SftpArea::kExports));
                handle->entries.emplace_back(FolderEntry("imports", SftpArea::kImports));
            }
            else
            {
                // Not there yet just means nothing's been exported or
                // uploaded: an empty folder.
                std::string dir = AreaDir(path.area);
                std::error_code error;
                for (const std::filesystem::directory_entry& item : std::filesystem::directory_iterator(dir, error))
                {
                    std::string name = item.path().filename().string();
                    struct stat info{};
                    if (name[0] == '.' || ::lstat(item.path().c_str(), &info) != 0 || !S_ISREG(info.st_mode))
                    {
                        continue;
                    }
                    handle->entries.emplace_back(FileEntry(name, path.area, info));
                }
                std::sort(handle->entries.begin(), handle->entries.end(), CompareNames);
            }
            ReplyHandle(message, std::move(handle));
        }

        static bool CompareNames(const SftpEntry& left, const SftpEntry& right)
        {
            return left.name < right.name;
        }

        void ReadDirectory(sftp_client_message message)
        {
            SftpHandle* handle = FindHandle(message);
            if (handle == nullptr)
            {
                return;
            }
            if (!handle->is_directory)
            {
                sftp_reply_status(message, SSH_FX_FAILURE, "Not a folder");
                return;
            }
            if (handle->next_entry >= handle->entries.size())
            {
                sftp_reply_status(message, SSH_FX_EOF, nullptr);
                return;
            }
            std::size_t end = std::min(handle->entries.size(), handle->next_entry + kNamesPerReply);
            for (; handle->next_entry < end; ++handle->next_entry)
            {
                const SftpEntry& entry = handle->entries[handle->next_entry];
                std::string long_name = SftpLongName(entry.name, entry.permissions, entry.size, entry.mtime, username_);
                sftp_attributes_struct attributes{};
                FillAttributes(entry, &attributes);
                sftp_reply_names_add(message, entry.name.c_str(), long_name.c_str(), &attributes);
            }
            sftp_reply_names(message);
        }

        void Open(sftp_client_message message)
        {
            SftpPath path;
            if (!ResolveMessagePath(message, &path))
            {
                return;
            }
            if (path.name.empty())
            {
                sftp_reply_status(message, SSH_FX_FAILURE, "That's a folder");
                return;
            }
            std::uint32_t flags = sftp_client_message_get_flags(message);
            if ((flags & SSH_FXF_WRITE) != 0)
            {
                OpenForWriting(message, path, flags);
                return;
            }

            int fd = ::open(RealPathOf(path).c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
            struct stat info{};
            if (fd < 0 || ::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode))
            {
                if (fd >= 0)
                {
                    ::close(fd);
                }
                sftp_reply_status(message, SSH_FX_NO_SUCH_FILE, "No such file");
                return;
            }
            std::unique_ptr<SftpHandle> handle = std::make_unique<SftpHandle>();
            handle->fd = fd;
            ReplyHandle(message, std::move(handle));
        }

        void OpenForWriting(sftp_client_message message, const SftpPath& path, std::uint32_t flags)
        {
            if (!AreaWritable(path.area))
            {
                Refuse(message, SSH_FX_PERMISSION_DENIED,
                       view_only_ && path.area == SftpArea::kImports ? "View-only users can't upload"
                                                                     : "That folder is read-only");
                return;
            }
            if (!IsAllowedImportName(path.name))
            {
                Refuse(message, SSH_FX_PERMISSION_DENIED, "Only .qlnet and .qlsession files can be uploaded");
                return;
            }

            std::string final_path = RealPathOf(path);
            struct stat existing{};
            bool exists = ::lstat(final_path.c_str(), &existing) == 0;
            if (exists && (flags & SSH_FXF_EXCL) != 0)
            {
                sftp_reply_status(message, SSH_FX_FILE_ALREADY_EXISTS, "Already exists");
                return;
            }
            // An upload always starts from nothing, in its own temporary
            // file, so leaving out TRUNC changes nothing: OpenSSH's scp and
            // sftp do, and trim the file with a setstat at the end instead.
            // Only a resume (APPEND here, or a write past the end, see
            // Write) can't be done.
            if ((flags & SSH_FXF_APPEND) != 0)
            {
                Refuse(message, SSH_FX_OP_UNSUPPORTED, "Can't resume an upload");
                return;
            }
            std::uint64_t limit = SftpUploadLimit(SftpImportsBytesUsed(imports_dir_, path.name));
            if (limit == 0)
            {
                Refuse(message, SSH_FX_FAILURE, "/imports is full (100 MB)");
                return;
            }
            if (!EnsureDirectory(imports_dir_))
            {
                sftp_reply_status(message, SSH_FX_FAILURE, "Can't create the folder");
                return;
            }

            // Hidden (see ResolveSftpPath) until it's whole.
            std::string temp_path = imports_dir_ + "/." + TemporaryPathFor(path.name);
            int fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
            if (fd < 0)
            {
                sftp_reply_status(message, SSH_FX_FAILURE, "Can't create the file");
                return;
            }
            std::unique_ptr<SftpHandle> handle = std::make_unique<SftpHandle>();
            handle->fd = fd;
            handle->writing = true;
            handle->temp_path = std::move(temp_path);
            handle->final_path = std::move(final_path);
            handle->limit = limit;
            ReplyHandle(message, std::move(handle));
        }

        void Read(sftp_client_message message)
        {
            SftpHandle* handle = FindHandle(message);
            if (handle == nullptr)
            {
                return;
            }
            if (handle->is_directory || handle->writing)
            {
                sftp_reply_status(message, SSH_FX_FAILURE, "Not open for reading");
                return;
            }
            std::uint32_t length = std::min(message->len, kMaxReadBytes);
            buffer_.resize(length);
            ssize_t count = ::pread(handle->fd, buffer_.data(), length, static_cast<off_t>(message->offset));
            if (count < 0)
            {
                sftp_reply_status(message, SSH_FX_FAILURE, "Can't read the file");
                return;
            }
            if (count == 0)
            {
                sftp_reply_status(message, SSH_FX_EOF, nullptr);
                return;
            }
            sftp_reply_data(message, buffer_.data(), static_cast<int>(count));
        }

        void Write(sftp_client_message message)
        {
            SftpHandle* handle = FindHandle(message);
            if (handle == nullptr)
            {
                return;
            }
            if (!handle->writing)
            {
                sftp_reply_status(message, SSH_FX_PERMISSION_DENIED, "Not open for writing");
                return;
            }
            if (handle->failed)
            {
                sftp_reply_status(message, SSH_FX_FAILURE, "Upload already failed");
                return;
            }
            const char* data = static_cast<const char*>(ssh_string_data(message->data));
            std::size_t length = ssh_string_len(message->data);
            if (message->offset > handle->limit || length > handle->limit - message->offset)
            {
                handle->failed = true;
                Refuse(message, SSH_FX_FAILURE,
                       handle->limit < kSftpMaxUploadBytes ? "/imports is full (100 MB)"
                                                           : "The file is over the 25 MB limit");
                return;
            }
            if (message->offset > handle->size)
            {
                handle->failed = true;
                Refuse(message, SSH_FX_OP_UNSUPPORTED, "Can't resume an upload");
                return;
            }
            std::size_t written = 0;
            while (written < length)
            {
                ssize_t count = ::pwrite(handle->fd, data + written, length - written,
                                         static_cast<off_t>(message->offset + written));
                if (count < 0 && errno == EINTR)
                {
                    continue;
                }
                if (count <= 0)
                {
                    handle->failed = true;
                    sftp_reply_status(message, SSH_FX_FAILURE, "Can't write the file");
                    return;
                }
                written += static_cast<std::size_t>(count);
            }
            handle->size = std::max(handle->size, message->offset + length);
            sftp_reply_status(message, SSH_FX_OK, nullptr);
        }

        void Close(sftp_client_message message)
        {
            SftpHandle* handle = FindHandle(message);
            if (handle == nullptr)
            {
                return;
            }
            sftp_handle_remove(sftp_, handle);

            bool upload_failed = handle->writing && handle->failed;
            bool replaced = true;
            std::string error;
            if (handle->writing && !handle->failed)
            {
                ::close(handle->fd);
                handle->fd = -1;
                replaced = ReplaceWithFile(handle->temp_path, handle->final_path, &error);
                handle->writing = false;
            }
            CloseFile(handle);
            Forget(handle);

            if (upload_failed)
            {
                sftp_reply_status(message, SSH_FX_FAILURE, "Upload failed; nothing was saved");
            }
            else if (!replaced)
            {
                sftp_reply_status(message, SSH_FX_FAILURE, "Can't save the file");
            }
            else
            {
                sftp_reply_status(message, SSH_FX_OK, nullptr);
            }
        }

        void Remove(sftp_client_message message)
        {
            SftpPath path;
            if (!ResolveMessagePath(message, &path))
            {
                return;
            }
            if (path.name.empty() || !AreaWritable(path.area) || !IsAllowedImportName(path.name))
            {
                Refuse(message, SSH_FX_PERMISSION_DENIED, "Only your uploads can be removed");
                return;
            }
            std::string real_path = RealPathOf(path);
            struct stat info{};
            if (::lstat(real_path.c_str(), &info) != 0 || !S_ISREG(info.st_mode))
            {
                sftp_reply_status(message, SSH_FX_NO_SUCH_FILE, "No such file");
                return;
            }
            if (::unlink(real_path.c_str()) != 0)
            {
                sftp_reply_status(message, SSH_FX_FAILURE, "Can't remove the file");
                return;
            }
            sftp_reply_status(message, SSH_FX_OK, nullptr);
        }

        // Permissions and times are QuickLogger's to decide, but scp sets
        // them after every upload (and fails the copy if it can't), so the
        // request is accepted and ignored.
        void SetStat(sftp_client_message message) const
        {
            if (sftp_client_message_get_type(message) == SSH_FXP_FSETSTAT)
            {
                if (FindHandle(message) != nullptr)
                {
                    sftp_reply_status(message, SSH_FX_OK, nullptr);
                }
                return;
            }
            SftpPath path;
            if (!ResolveMessagePath(message, &path))
            {
                return;
            }
            SftpEntry entry;
            if (!Lookup(path, &entry))
            {
                sftp_reply_status(message, SSH_FX_NO_SUCH_FILE, "No such file");
                return;
            }
            sftp_reply_status(message, SSH_FX_OK, nullptr);
        }

        // Closes a handle's file, if it has one. An upload that never made
        // it into place is deleted.
        static void CloseFile(SftpHandle* handle)
        {
            if (handle->fd >= 0)
            {
                ::close(handle->fd);
                handle->fd = -1;
            }
            if (handle->writing)
            {
                ::unlink(handle->temp_path.c_str());
                handle->writing = false;
            }
        }

        void Forget(SftpHandle* handle)
        {
            for (std::size_t i = 0; i < handles_.size(); ++i)
            {
                if (handles_[i].get() == handle)
                {
                    handles_.erase(handles_.begin() + static_cast<std::ptrdiff_t>(i));
                    return;
                }
            }
        }

        // Refuses a request and says why. OpenSSH's sftp and scp show only
        // the status code's own words ("Failure", "Permission denied") for
        // a reply, never its message, but pass what the server writes to the
        // channel's standard error through to the person's terminal: so
        // the reason goes there too (and push reads it, see
        // ConnectionFailure in upstream_push.cpp).
        void Refuse(sftp_client_message message, std::uint32_t status, const std::string& reason)
        {
            std::string line = reason + "\n";
            ssh_channel_write_stderr(channel_, line.data(), static_cast<std::uint32_t>(line.size()));
            sftp_reply_status(message, status, reason.c_str());
        }

        sftp_session sftp_;
        ssh_channel channel_;
        std::string exports_dir_;
        std::string imports_dir_;
        std::string username_;
        bool view_only_;
        std::vector<std::unique_ptr<SftpHandle>> handles_;
        std::vector<char> buffer_;
    };

    void RunSftpSession(ssh_session session, ssh_channel channel, const std::string& db_path,
                        const std::string& username, bool view_only)
    {
        sftp_session sftp = sftp_server_new(session, channel);
        if (sftp == nullptr)
        {
            return;
        }
        // sftp_server_init is deprecated from libssh 0.11 in favor of a
        // callback API that 0.10 (Debian 12, Ubuntu 24.04) doesn't have.
        // Like ssh_pki_generate in ssh_server.cpp, kept on the function
        // every supported libssh has.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
        bool initialized = sftp_server_init(sftp) == 0;
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        if (!initialized)
        {
            sftp_server_free(sftp);
            return;
        }

        {
            SftpSession files(sftp, channel, db_path, username, view_only);
            while (true)
            {
                sftp_client_message message = sftp_get_client_message(sftp);
                if (message == nullptr)
                {
                    break;
                }
                files.Handle(message);
                sftp_client_message_free(message);
            }
        }
        sftp_server_free(sftp);
    }

}  // namespace ql
