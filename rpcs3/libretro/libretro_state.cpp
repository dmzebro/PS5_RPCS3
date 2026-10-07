#include "stdafx.h"
#include "libretro_state.h"

#include "Utilities/File.h"

#include <ctime>
#include <map>
#include <mutex>
#include <set>

namespace libretro_state
{
	namespace
	{
		// fs::get_virtual_device takes a path whose device name follows
		// "/vfsv0_virtual_", with '_' at index 29 (Utilities/File.cpp).
		const std::string device_name = "libretro_state_mem";
		const std::string root = "/vfsv0_virtual_" + device_name;

		using bytes_ptr = std::shared_ptr<std::vector<u8>>;

		class memory_file final : public fs::file_base
		{
			bytes_ptr m_bytes;
			u64 m_pos = 0;
			bool m_writable;

		public:
			memory_file(bytes_ptr bytes, bool writable)
				: m_bytes(std::move(bytes)), m_writable(writable)
			{
			}

			fs::stat_t get_stat() override
			{
				fs::stat_t info{};
				info.size = m_bytes->size();
				info.is_writable = m_writable;
				info.atime = info.mtime = info.ctime = std::time(nullptr);
				return info;
			}

			bool trunc(u64 length) override
			{
				m_bytes->resize(length);
				return true;
			}

			u64 read(void* buffer, u64 size) override
			{
				const u64 done = read_at(m_pos, buffer, size);
				m_pos += done;
				return done;
			}

			u64 read_at(u64 offset, void* buffer, u64 size) override
			{
				if (offset >= m_bytes->size())
					return 0;
				const u64 done = std::min<u64>(size, m_bytes->size() - offset);
				std::memcpy(buffer, m_bytes->data() + offset, done);
				return done;
			}

			u64 write(const void* buffer, u64 size) override
			{
				if (!m_writable)
					return 0;
				if (m_pos + size > m_bytes->size())
					m_bytes->resize(m_pos + size);
				std::memcpy(m_bytes->data() + m_pos, buffer, size);
				m_pos += size;
				return size;
			}

			u64 seek(s64 offset, fs::seek_mode whence) override
			{
				const s64 base = whence == fs::seek_set ? 0 : whence == fs::seek_cur ? static_cast<s64>(m_pos) : static_cast<s64>(m_bytes->size());
				if (base + offset < 0)
				{
					fs::g_tls_error = fs::error::inval;
					return -1;
				}
				m_pos = static_cast<u64>(base + offset);
				return m_pos;
			}

			u64 size() override
			{
				return m_bytes->size();
			}
		};

		class memory_device final : public fs::device_base
		{
			std::mutex m_lock;
			std::map<std::string, bytes_ptr> m_files;
			std::set<std::string> m_dirs{root};

			static std::string trimmed(std::string path)
			{
				while (path.size() > root.size() && path.back() == '/')
					path.pop_back();
				return path;
			}

		public:
			memory_device()
			{
				fs_prefix = root;
			}

			bool stat(const std::string& path, fs::stat_t& info) override
			{
				std::lock_guard lock(m_lock);
				info = {};
				info.is_writable = true;
				info.atime = info.mtime = info.ctime = std::time(nullptr);
				if (const auto found = m_files.find(path); found != m_files.end())
				{
					info.size = found->second->size();
					return true;
				}
				if (m_dirs.contains(trimmed(path)))
				{
					info.is_directory = true;
					return true;
				}
				fs::g_tls_error = fs::error::noent;
				return false;
			}

			bool statfs(const std::string&, fs::device_stat& info) override
			{
				info = {};
				info.block_size = 4096;
				info.total_size = info.total_free = info.avail_free = u64{1} << 40;
				return true;
			}

			bool create_dir(const std::string& path) override
			{
				std::lock_guard lock(m_lock);
				m_dirs.insert(trimmed(path));
				return true;
			}

			bool remove(const std::string& path) override
			{
				std::lock_guard lock(m_lock);
				if (m_files.erase(path))
					return true;
				fs::g_tls_error = fs::error::noent;
				return false;
			}

			bool rename(const std::string& from, const std::string& to) override
			{
				std::lock_guard lock(m_lock);
				const auto found = m_files.find(from);
				if (found == m_files.end())
				{
					fs::g_tls_error = fs::error::noent;
					return false;
				}
				bytes_ptr bytes = std::move(found->second);
				m_files.erase(found);
				m_files[to] = std::move(bytes);
				return true;
			}

			bool trunc(const std::string& path, u64 length) override
			{
				std::lock_guard lock(m_lock);
				const auto found = m_files.find(path);
				if (found == m_files.end())
				{
					fs::g_tls_error = fs::error::noent;
					return false;
				}
				found->second->resize(length);
				return true;
			}

			bool utime(const std::string&, s64, s64) override
			{
				return true;
			}

			std::unique_ptr<fs::file_base> open(const std::string& path, bs_t<fs::open_mode> mode) override
			{
				std::lock_guard lock(m_lock);
				auto found = m_files.find(path);
				if (found == m_files.end())
				{
					if (!(mode & fs::create))
					{
						fs::g_tls_error = fs::error::noent;
						return nullptr;
					}
					found = m_files.emplace(path, std::make_shared<std::vector<u8>>()).first;
				}
				else if (mode & fs::excl)
				{
					fs::g_tls_error = fs::error::exist;
					return nullptr;
				}
				if (mode & fs::trunc)
					found->second->clear();
				auto file = std::make_unique<memory_file>(found->second, !!(mode & fs::write));
				if (mode & fs::append)
					file->seek(0, fs::seek_end);
				return file;
			}

			std::unique_ptr<fs::dir_base> open_dir(const std::string&) override
			{
				fs::g_tls_error = fs::error::noent;
				return nullptr;
			}

			std::vector<u8> copy(const std::string& path)
			{
				std::lock_guard lock(m_lock);
				const auto found = m_files.find(path);
				return found == m_files.end() ? std::vector<u8>{} : *found->second;
			}

			void put(const std::string& path, std::vector<u8> bytes)
			{
				std::lock_guard lock(m_lock);
				m_files[path] = std::make_shared<std::vector<u8>>(std::move(bytes));
			}

			void drop(const std::string& path)
			{
				std::lock_guard lock(m_lock);
				m_files.erase(path);
			}
		};

		memory_device& device()
		{
			static const auto mounted = []
			{
				auto made = stx::make_shared<memory_device>();
				fs::set_virtual_device(device_name, made);
				return made;
			}();
			return *mounted;
		}
	}

	std::string path(std::string_view name)
	{
		device();
		return root + "/" + std::string(name);
	}

	std::vector<u8> read(const std::string& path)
	{
		return device().copy(path);
	}

	void write(const std::string& path, std::vector<u8> bytes)
	{
		device().put(path, std::move(bytes));
	}

	void remove(const std::string& path)
	{
		device().drop(path);
	}
}
