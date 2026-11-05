// Declarations implemented in this file
#include "app_log.hpp"

// std::chrono::system_clock: the current wall-clock time for the timestamp
#include <chrono>
// std::snprintf: format the milliseconds
#include <cstdio>
// std::tm, std::strftime, localtime_r: format the timestamp
#include <ctime>
// std::filesystem: resolve relative paths, create the log folder
#include <filesystem>
// std::map: id → message
#include <map>
// std::runtime_error: validation errors while loading the catalogue
#include <stdexcept>

// Google glog: LogMessage, the FLAGS_* settings, SetLogDestination
#include <glog/logging.h>
// yaml-cpp: read configs/log_messages.yaml
#include <yaml-cpp/yaml.h>

// Namespace alias
namespace fs = std::filesystem;

// Anonymous namespace: only visible inside this file
namespace {

// One entry of the catalogue (one item under messages: in the yaml file)
struct Message {
    // [code]
    std::string code = "0000";
    // [level]: "info", "warning" or "error"
    std::string level = "error";
    // [module]
    std::string module = "log";
    // [function] override; "" = use the C++ function name
    std::string function;
    // Text with {placeholders}
    std::string description;
};

// The settings: block of the yaml file
struct Settings {
    // strftime() format of the timestamp
    std::string timestamp_format = "%Y-%m-%d %H:%M:%S";
    // Append ".123" milliseconds to the timestamp
    bool milliseconds = true;
    // Lowest level that is written: info / warning / error
    std::string min_level = "info";
    // Manager: also show the log lines in the terminal
    bool console = true;
    // Manager: colour warnings (yellow) and errors (red) in the terminal
    bool color = true;
    // Manager: log file (absolute after loading); "" = no file
    std::string manager_log_file;
};

// The loaded catalogue: id → message
std::map<std::string, Message> g_messages;
// The loaded settings
Settings g_settings;
// Init() has already run (glog may only be initialised once per process)
bool g_initialised = false;

// Built-in message used when the catalogue itself cannot be loaded (it cannot come from the yaml file)
Message CatalogFailedMessage() {
    // Start with the defaults
    Message m;
    // Fixed code
    m.code = "9001";
    // Error level
    m.level = "error";
    // The log module
    m.module = "log";
    // Text
    m.description = "cannot load log catalogue {path}: {reason} - using built-in defaults";
    // Return it
    return m;
}

// Built-in message used when an id is missing from the catalogue (overridable with LOG_UNKNOWN_ID in the yaml file)
Message UnknownIdMessage() {
    // Start with the defaults
    Message m;
    // Fixed code
    m.code = "9002";
    // Error level
    m.level = "error";
    // The log module
    m.module = "log";
    // Text: {id} is the missing id, {params} the values that were passed
    m.description = "unknown log id '{id}' {params}";
    // Return it
    return m;
}

// Level text → glog severity (INFO = 0, WARNING = 1, ERROR = 2)
google::LogSeverity Severity(const std::string& level) {
    // warning
    if (level == "warning") return google::GLOG_WARNING;
    // error
    if (level == "error") return google::GLOG_ERROR;
    // everything else is treated as info
    return google::GLOG_INFO;
}

// Is the level text one of the three accepted values
bool IsValidLevel(const std::string& level) {
    // info / warning / error
    return level == "info" || level == "warning" || level == "error";
}

// Replace every "{key}" in text with its value
std::string Fill(std::string text, const app_log::Params& params) {
    // For each placeholder value
    for (const app_log::Param& p : params) {
        // The placeholder as written in the yaml file, e.g. "{name}"
        const std::string key = "{" + p.first + "}";
        // Where the next search starts
        size_t pos = 0;
        // Replace every occurrence
        while ((pos = text.find(key, pos)) != std::string::npos) {
            // Swap the placeholder for the value
            text.replace(pos, key.size(), p.second);
            // Continue after the inserted value (so a value containing "{name}" is not replaced again)
            pos += p.second.size();
        }
    }
    // The filled-in text
    return text;
}

// All parameters as "{k=v, k=v}" (used when the id is unknown, so the information is not lost)
std::string ListParams(const app_log::Params& params) {
    // Opening brace
    std::string out = "{";
    // Each key=value
    for (size_t i = 0; i < params.size(); ++i) {
        // Separator between items
        if (i > 0) out += ", ";
        // key=value
        out += params[i].first + "=" + params[i].second;
    }
    // Closing brace
    return out + "}";
}

// The current local time formatted with settings.timestamp_format (+ ".mmm")
std::string Timestamp() {
    // Now
    const std::chrono::system_clock::time_point now = std::chrono::system_clock::now();
    // As seconds since 1970 (what the C time functions use)
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    // Broken-down local time
    std::tm local {};
    // localtime_r is the thread-safe version of localtime
    localtime_r(&seconds, &local);
    // Buffer for the formatted text
    char buf[64];
    // Format; returns 0 if the result did not fit
    const size_t n = std::strftime(buf, sizeof buf, g_settings.timestamp_format.c_str(), &local);
    // The formatted text (empty if it did not fit)
    std::string text(buf, n);
    // Without milliseconds we are done
    if (!g_settings.milliseconds) return text;
    // Milliseconds within the current second
    const long ms = static_cast<long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
    // Buffer for ".123"
    char frac[8];
    // Always three digits
    std::snprintf(frac, sizeof frac, ".%03ld", ms);
    // Append
    return text + frac;
}

// Build the full line and send it to glog
void Emit(const Message& m, const char* function, const std::string& description) {
    // [function]: the yaml override if given, otherwise the C++ function name
    const std::string fn = m.function.empty() ? std::string(function ? function : "-") : m.function;
    // timestamp [level] [code] [module] [function] description
    const std::string line = Timestamp() + " [" + m.level + "] [" + m.code + "] [" + m.module + "] [" + fn + "] " + description;
    // Hand it to glog with the matching severity. glog's own prefix is switched off (FLAGS_log_prefix),
    // so the line is written exactly as built above. "app_log" / 0 are the source file / line glog requires.
    google::LogMessage("app_log", 0, Severity(m.level)).stream() << line;
}

// Read a string field if present
void ReadString(const YAML::Node& node, const char* key, std::string& out) {
    // Only overwrite when the key exists
    if (node[key]) out = node[key].as<std::string>();
}

// Read a bool field if present
void ReadBool(const YAML::Node& node, const char* key, bool& out) {
    // Only overwrite when the key exists
    if (node[key]) out = node[key].as<bool>();
}

// Load the yaml file into settings / messages; throws YAML::Exception or std::runtime_error on failure
void LoadCatalog(const std::string& path, Settings& settings, std::map<std::string, Message>& messages) {
    // Parse the whole file (throws if missing or invalid)
    const YAML::Node root = YAML::LoadFile(path);
    // Relative paths are relative to the folder holding the yaml file
    const fs::path base = fs::absolute(path).parent_path();

    // The settings: block (optional; missing keys keep their defaults)
    const YAML::Node s = root["settings"];
    // Present
    if (s) {
        // Timestamp format
        ReadString(s, "timestamp_format", settings.timestamp_format);
        // Milliseconds on / off
        ReadBool(s, "milliseconds", settings.milliseconds);
        // Lowest level written
        ReadString(s, "min_level", settings.min_level);
        // Terminal output on / off
        ReadBool(s, "console", settings.console);
        // Colour on / off
        ReadBool(s, "color", settings.color);
        // Manager log file
        ReadString(s, "manager_log_file", settings.manager_log_file);
    }
    // min_level must be a valid level
    if (!IsValidLevel(settings.min_level)) {
        // Report which value was wrong
        throw std::runtime_error("settings.min_level must be info, warning or error (got '" + settings.min_level + "')");
    }
    // Make the log file path absolute (relative to the yaml folder)
    if (!settings.manager_log_file.empty() && fs::path(settings.manager_log_file).is_relative()) {
        // Combine and tidy up (e.g. "configs/../logs" → "logs")
        settings.manager_log_file = fs::weakly_canonical(base / settings.manager_log_file).string();
    }

    // The messages: block (required)
    const YAML::Node list = root["messages"];
    // Missing or not a map
    if (!list || !list.IsMap()) throw std::runtime_error("missing 'messages:' map");
    // Each message (item->first = id, item->second = fields)
    for (YAML::const_iterator item = list.begin(); item != list.end(); ++item) {
        // The id used in the source code
        const std::string id = item->first.as<std::string>();
        // Its fields
        const YAML::Node node = item->second;
        // The entry being built
        Message m;
        // [code] (numbers are read as text, so 0042 keeps its leading zeros only if quoted)
        ReadString(node, "code", m.code);
        // [level]
        ReadString(node, "level", m.level);
        // [module]
        ReadString(node, "module", m.module);
        // Optional [function] override
        ReadString(node, "function", m.function);
        // Text
        ReadString(node, "description", m.description);
        // The level must be valid
        if (!IsValidLevel(m.level)) {
            // Report which message was wrong
            throw std::runtime_error("message '" + id + "': level must be info, warning or error (got '" + m.level + "')");
        }
        // Store it
        messages[id] = m;
    }
}

// glog sink that appends each line to the manager log file.
// glog's own log files are not used: glog 0.6 always writes a 4-line "Log file created at ..." header into them,
// which would break the rule that every line starts with a timestamp. A sink receives only the message text.
class FileSink : public google::LogSink {
public:
    // Open the file in append mode ("a"): earlier runs are kept
    explicit FileSink(const std::string& path) : file_(std::fopen(path.c_str(), "a")) {}
    // Close the file
    ~FileSink() override {
        // Only if it was opened
        if (file_) std::fclose(file_);
    }
    // Keep the base class's other (deprecated) send() overload visible
    using google::LogSink::send;
    // Called by glog for every line that passes FLAGS_minloglevel (glog holds its lock, so no LOG() in here)
    void send(google::LogSeverity, const char*, const char*, int, const google::LogMessageTime&,
              const char* message, size_t length) override {
        // File could not be opened: drop the line
        if (!file_) return;
        // The line exactly as built by Emit()
        std::fwrite(message, 1, length, file_);
        // glog passes it without the newline
        std::fputc('\n', file_);
        // Write it out now, so the file is up to date even if the program crashes
        std::fflush(file_);
    }
private:
    // The open log file (nullptr if opening failed)
    FILE* file_;
};

// The manager's file sink; lives until the program ends (glog keeps a pointer to it)
FileSink* g_file_sink = nullptr;

// Apply the settings to glog
void ConfigureGlog(app_log::Output output) {
    // No glog prefix ("I1104 16:29:59.123 1234 file.cc:12]"): our line already has the timestamp and level
    FLAGS_log_prefix = false;
    // Write every line immediately (by default glog buffers INFO lines for up to 30 s)
    FLAGS_logbuflevel = -1;
    // Do not wait before flushing
    FLAGS_logbufsecs = 0;
    // Lowest level that is written
    FLAGS_minloglevel = Severity(g_settings.min_level);
    // Use the log file name exactly as given (no date and pid appended)
    FLAGS_timestamp_in_logfile_name = false;

    // Child process: everything goes to stderr, which the manager has redirected into logs/<name>.log
    if (output == app_log::Output::StandardError) {
        // stderr only, no glog files
        FLAGS_logtostderr = true;
        // No colour codes inside the log file
        FLAGS_colorlogtostderr = false;
        // Done
        return;
    }

    // Manager: files are handled below, so do not send everything to stderr only
    FLAGS_logtostderr = false;
    // Copy every line to the terminal if settings.console is true
    FLAGS_alsologtostderr = g_settings.console;
    // Without console output, nothing below FATAL goes to stderr
    FLAGS_stderrthreshold = g_settings.console ? google::GLOG_INFO : google::GLOG_FATAL;
    // Colour in the terminal
    FLAGS_colorlogtostderr = g_settings.color;
    // Switch off glog's own log files for every level (an empty destination disables a file); FileSink writes ours
    google::SetLogDestination(google::GLOG_INFO, "");
    // No WARNING file
    google::SetLogDestination(google::GLOG_WARNING, "");
    // No ERROR file
    google::SetLogDestination(google::GLOG_ERROR, "");
    // No FATAL file
    google::SetLogDestination(google::GLOG_FATAL, "");
    // No log file configured: terminal only
    if (g_settings.manager_log_file.empty()) return;
    // Error code (non-throwing version)
    std::error_code ec;
    // Make sure the folder exists
    fs::create_directories(fs::path(g_settings.manager_log_file).parent_path(), ec);
    // Open the file; deliberately never deleted, because glog may still call it while the program shuts down
    g_file_sink = new FileSink(g_settings.manager_log_file);
    // Register it: from now on glog hands every line to it as well
    google::AddLogSink(g_file_sink);
}

}  // namespace

// Load the catalogue and initialise glog
bool app_log::Init(const std::string& catalog_path, const char* program_name, Output output) {
    // glog may only be initialised once
    if (g_initialised) return true;
    // Remember that it has been done
    g_initialised = true;

    // Load into temporaries first, so a broken file leaves the built-in defaults untouched
    Settings settings;
    // Temporary catalogue
    std::map<std::string, Message> messages;
    // Why loading failed ("" = success)
    std::string reason;
    // yaml-cpp and LoadCatalog throw on errors
    try {
        // Read the file
        LoadCatalog(catalog_path, settings, messages);
        // Success: use the loaded values
        g_settings = settings;
        // Success: use the loaded messages
        g_messages = messages;
    // yaml syntax error, missing file, wrong field type
    } catch (const YAML::Exception& e) {
        // Keep the reason
        reason = e.what();
    // Our own validation errors
    } catch (const std::exception& e) {
        // Keep the reason
        reason = e.what();
    }

    // Apply the settings (or the defaults) to glog
    ConfigureGlog(output);
    // Start glog; program_name is only used internally by glog
    google::InitGoogleLogging(program_name);

    // Loaded fine
    if (reason.empty()) return true;
    // Report the failure with the built-in message
    Emit(CatalogFailedMessage(), __func__, Fill(CatalogFailedMessage().description, {{"path", catalog_path}, {"reason", reason}}));
    // Failed
    return false;
}

// Write one log line
void app_log::Write(const char* id, const char* function, const Params& params) {
    // Look up the id
    const std::map<std::string, Message>::const_iterator it = g_messages.find(id);
    // Found: fill in its description and write it
    if (it != g_messages.end()) {
        // Write the line
        Emit(it->second, function, Fill(it->second.description, params));
        // Done
        return;
    }
    // Not found: use LOG_UNKNOWN_ID from the yaml file if it exists, otherwise the built-in one
    const std::map<std::string, Message>::const_iterator unknown = g_messages.find("LOG_UNKNOWN_ID");
    // The message to use
    const Message m = unknown != g_messages.end() ? unknown->second : UnknownIdMessage();
    // Write it, with the missing id and every value that was passed
    Emit(m, function, Fill(m.description, {{"id", id}, {"params", ListParams(params)}}));
}
