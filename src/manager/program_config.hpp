#ifndef PROGRAM_CONFIG_HPP
#define PROGRAM_CONFIG_HPP

// std::map
#include <map>
// std::string
#include <string>
// std::vector
#include <vector>


// Struct for a program: .so to load, default arguments, working directory
struct ProgramDef {
    // .so absolute path (or relative to the YAML file)
    std::string library;
    // Default arguments for the program (optional)
    std::vector<std::string> args;
    //  Working directory for the program; default is the current working directory (optional)
    std::string working_dir;
};


// Program name → definition (std::map is sorted by name)
using ProgramTable = std::map<std::string, ProgramDef>;

//   Load programs from a YAML file. The YAML file should look like this:
//
//   programs:
//     isurgical:
//       library: ../build/libisurgical_plugin.so              # required, .so path
//       working_dir: ..                                       # optional
//       args: ["--config", "configs/isurgical_config.yaml"]   # optional
//
// Errors are logged where they happen (CFG_* in configs/log_messages.yaml), so there is no error string.
bool LoadPrograms(const std::string& path, ProgramTable& programs);

#endif // PROGRAM_CONFIG_HPP