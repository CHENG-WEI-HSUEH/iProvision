#include "app_config.hpp"

// std::filesystem： read symlink, combine path
#include <filesystem>

// Rename filesystem as fs
namespace fs = std::filesystem;

namespace app_config {


  // Implementation: Convert a path relative to the executable into an absolute path
  std::string ResolveFromExecutable(const char* relative_path) {

      // Error code:
      // std::errc::interrupted 
      // std::errc::no_such_file_or_directory
      // std::errc::permission_denied
      // std::errc::timeout
      std::error_code ec;

      // Symbolic link: /proc/self/exe → /path/to/iProvision/build/iprovision
      const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
      // If the symbolic link cannot be read, use the current working directory as the base path
      const fs::path base = ec ? fs::current_path() : exe.parent_path();

      // Combine the base path with the relative path and return the weakly canonical absolute path as a string
      return fs::weakly_canonical(base / relative_path).string();
  }


}  // namespace app_config
