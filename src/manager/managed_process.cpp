// 這個檔案實作的類別宣告（也會先定義 BOOST_PROCESS_USE_STD_FS）
#include "managed_process.hpp"

// errno：系統呼叫失敗時的錯誤編號
#include <cerrno>
// kill、SIGSTOP、SIGCONT、SIGKILL 等訊號
#include <csignal>
// std::fopen、std::fclose：open the child's log file
#include <cstdio>
// std::strerror：把 errno 轉成文字
#include <cstring>
// std::filesystem：路徑處理
#include <filesystem>
// std::ifstream：讀 /proc/<pid>/stat
#include <fstream>
// std::this_thread::sleep_for
#include <thread>
// prctl、PR_SET_PDEATHSIG：父行程死掉時通知子行程
#include <sys/prctl.h>
// WIFEXITED、WEXITSTATUS 等：解讀結束狀態
#include <sys/wait.h>
// getpid、getppid、setpgid、access、_exit
#include <unistd.h>

// bp::environment::find_executable：在 PATH 裡找執行檔
#include <boost/process/v2/environment.hpp>
// bp::process_start_dir：設定子行程的工作目錄
#include <boost/process/v2/start_dir.hpp>
// bp::process_stdio：設定子行程的 stdin / stdout / stderr
#include <boost/process/v2/stdio.hpp>

// APP_LOG: write a line from configs/log_messages.yaml
#include "app_log.hpp"

// 命名空間別名
namespace bp = boost::process::v2;
// 單調時鐘：不受系統時間調整影響，適合量測經過時間
using Clock = std::chrono::steady_clock;

// 狀態 → 文字
const char* ToString(ProcessState state) {
    // 依狀態回傳對應文字
    switch (state) {
        // 正在執行
        case ProcessState::Running:  return "running";
        // 已暫停
        case ProcessState::Paused:   return "paused";
        // 停止中
        case ProcessState::Stopping: return "stopping";
        // 已結束
        case ProcessState::Exited:   return "exited";
    }
    // 不會到這裡（只是讓編譯器不警告）
    return "?";
}

// 匿名命名空間：只在這個檔案內使用
namespace {

// Boost.Process 的「初始化器」：Boost.Process 會在 fork 之後、exec 之前，在「子行程」裡呼叫
// on_exec_setup()。這裡做兩件事：
//  1. setpgid(0, 0)：子行程自成一個行程群組。在終端機按 Ctrl+C 時，SIGINT 只會送給管理器
//     （終端機會送給整個前景群組），由管理器負責乾淨地停止子行程。
//  2. PR_SET_PDEATHSIG = SIGKILL：管理器意外死掉時，核心會對子行程送 SIGKILL，子行程不會變成孤兒。
//     用 SIGKILL 而不是 SIGTERM：暫停中（SIGSTOP）的行程收到 SIGTERM 不會處理，會永遠卡住。
//     注意：這個訊號是在「呼叫 fork 的那個執行緒」結束時觸發，所以要在主執行緒啟動子行程。
struct DieWithParent {
    // 父行程（管理器）的 PID；在父行程建立這個物件時取得
    pid_t parent = ::getpid();

    // Boost.Process 在子行程 exec 之前呼叫；回傳錯誤碼會讓啟動失敗
    template <typename Launcher>
    bp::error_code on_exec_setup(Launcher&, const bp::filesystem::path&, const char* const*&) {
        // 成為新行程群組的組長（群組編號 = 自己的 PID）
        ::setpgid(0, 0);
        // 設定「父行程死掉時送我 SIGKILL」；失敗就回傳錯誤碼
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0) return {errno, boost::system::system_category()};
        // 如果在設定之前父行程就已經死了（父行程變了），直接結束，避免成為孤兒
        if (::getppid() != parent) ::_exit(127);
        // 成功
        return {};
    }
};

// 把時間長度轉成秒（浮點數）
double Seconds(Clock::duration d) {
    // 轉成 double 秒
    return std::chrono::duration<double>(d).count();
}

// Open the child's log file in append mode; nullptr on failure.
// No header line is written here any more: so_runner logs RUNNER_LOADING as the first line of every run.
FILE* OpenLog(const std::string& path) {
    // "a": append mode, earlier runs are kept
    return std::fopen(path.c_str(), "a");
}

}  // namespace

// 建構子：只記下 io_context 和名字，還不啟動
ManagedProcess::ManagedProcess(boost::asio::io_context& context, std::string name)
    : context_(context), name_(std::move(name)) {}

// 解構子
ManagedProcess::~ManagedProcess() {
    // 已啟動而且還沒結束，就先停止它（否則 Boost.Process 的解構子會直接 SIGKILL）
    if (process_ && !Exited()) Stop();
}

// 啟動子行程
bool ManagedProcess::Start(const ProcessSpec& spec) {
    // 一個 ManagedProcess 只能啟動一次
    if (process_) {
        // Log the reason
        APP_LOG("PROC_ALREADY_STARTED", {{"name", name_}});
        // 失敗
        return false;
    }

    // 決定要執行的檔案
    std::filesystem::path exe = spec.executable;
    // 沒有 '/' 的話（例如 "ping"），到 PATH 環境變數列出的資料夾裡找
    if (spec.executable.find('/') == std::string::npos) {
        // 在 PATH 裡找；找不到回傳空路徑
        exe = bp::environment::find_executable(spec.executable);
        // 找不到
        if (exe.empty()) {
            // Log the reason
            APP_LOG("PROC_NOT_IN_PATH", {{"name", name_}, {"exe", spec.executable}});
            // 失敗
            return false;
        }
    }
    // 錯誤碼（不丟例外的版本）
    std::error_code fs_ec;
    // 轉成絕對路徑：子行程會先切換到 working_dir 再 exec，相對路徑會找錯地方，所以要在這裡先轉好
    exe = std::filesystem::absolute(exe, fs_ec);
    // 檢查檔案存在而且可以執行（X_OK）
    if (::access(exe.c_str(), X_OK) != 0) {
        // Save errno first: building the log strings may overwrite it
        const int err = errno;
        // Log the reason, e.g. "No such file or directory", "Permission denied"
        APP_LOG("PROC_NOT_EXECUTABLE", {{"name", name_}, {"exe", exe.string()}, {"reason", std::strerror(err)}});
        // 失敗
        return false;
    }
    // 工作目錄：沒指定就用管理器目前的目錄
    const std::string working_dir =
        spec.working_dir.empty() ? std::filesystem::current_path(fs_ec).string() : spec.working_dir;

    // log 檔（沒指定 log_path 就是 nullptr，輸出會被丟棄）
    FILE* log = nullptr;
    // 有指定 log 檔
    if (!spec.log_path.empty()) {
        // 打開 log（append）
        log = OpenLog(spec.log_path);
        // 打不開
        if (!log) {
            // Save errno first: building the log strings may overwrite it
            const int err = errno;
            // Log the reason
            APP_LOG("PROC_LOG_OPEN_FAILED", {{"name", name_}, {"log", spec.log_path}, {"reason", std::strerror(err)}});
            // 失敗
            return false;
        }
    }

    // Boost.Process 啟動失敗時會丟出例外，用 try / catch 接住
    try {
        // 標準輸入輸出：stdin 接到 /dev/null（nullptr）；stdout、stderr 都寫到 log（沒有 log 就丟棄）
        bp::process_stdio stdio = log ? bp::process_stdio{nullptr, log, log}
                                      : bp::process_stdio{nullptr, nullptr, nullptr};
        // 真正啟動子行程（fork + exec）：執行檔、參數、工作目錄、輸入輸出、DieWithParent 初始化器
        process_ = std::make_unique<bp::process>(context_, exe, spec.args,
                                                 bp::process_start_dir(working_dir), stdio, DieWithParent{});
    // 啟動失敗（例如 exec 失敗）
    } catch (const boost::system::system_error& e) {
        // Log the reason
        APP_LOG("PROC_SPAWN_FAILED", {{"name", name_}, {"exe", exe.string()}, {"reason", e.what()}});
        // 關掉 log 檔
        if (log) std::fclose(log);
        // 失敗
        return false;
    }
    // 子行程已經有自己的一份檔案描述符，管理器這邊的可以關掉了
    if (log) std::fclose(log);

    // 記下啟動設定
    spec_ = spec;
    // 存絕對路徑，狀態顯示時比較清楚
    spec_.executable = exe.string();
    // 存實際使用的工作目錄
    spec_.working_dir = working_dir;
    // 子行程的 PID
    pid_ = process_->id();
    // 啟動時間
    started_at_ = Clock::now();
    // 重設狀態旗標
    stopping_ = exited_ = false;
    // 還沒有結束碼
    exit_code_ = -1;
    // 成功
    return true;
}

// 檢查子行程是否已結束
bool ManagedProcess::Poll() {
    // 沒啟動或已經結束：不在執行
    if (!process_ || exited_) return false;
    // 錯誤碼
    bp::error_code ec;
    // running() 內部是 waitpid(WNOHANG)：不會等待，只檢查。沒加 WUNTRACED，所以「暫停中」也算在執行
    if (process_->running(ec)) return true;
    // 走到這裡就是已經結束了
    exited_ = true;
    // 記下結束時間
    exited_at_ = Clock::now();
    // 預設結束碼不明（例如 ec 是 ECHILD：已經被別人回收了）
    exit_code_ = -1;
    // 正常取得結束狀態
    if (!ec) {
        // waitpid 回傳的原始狀態值
        const int status = process_->native_exit_code();
        // 正常 return / exit()：取出結束碼
        if (WIFEXITED(status)) exit_code_ = WEXITSTATUS(status);
        // 被訊號結束：慣例是 128 + 訊號編號
        else if (WIFSIGNALED(status)) exit_code_ = 128 + WTERMSIG(status);
    }
    // 不在執行了
    return false;
}

// 是否已結束
bool ManagedProcess::Exited() {
    // 先檢查一次最新狀態
    Poll();
    // 沒啟動，或已結束
    return !process_ || exited_;
}

// 等待子行程結束，最多 timeout
bool ManagedProcess::WaitExit(std::chrono::milliseconds timeout) {
    // 截止時間
    const Clock::time_point deadline = Clock::now() + timeout;
    // 還在執行就繼續等
    while (Poll()) {
        // 超過截止時間：放棄等待
        if (Clock::now() >= deadline) return false;
        // 睡一下再檢查，避免空轉吃 CPU
        std::this_thread::sleep_for(kPollInterval);
    }
    // 已結束
    return true;
}

// 讀出核心回報的行程狀態字母
char ManagedProcess::KernelState() const {
    // /proc/<pid>/stat 的格式：「pid (程式名) 狀態 ...」
    std::ifstream file("/proc/" + std::to_string(pid_) + "/stat");
    // 存放讀到的那一行
    std::string line;
    // 讀不到（行程已經不存在）
    if (!std::getline(file, line)) return 0;
    // 程式名可能包含空白或 ')'，所以找「最後一個」')'
    const size_t paren = line.rfind(')');
    // ')' 後面隔一個空白就是狀態字母
    return (paren != std::string::npos && paren + 2 < line.size()) ? line[paren + 2] : 0;
}

// 等到核心回報的狀態符合期望
bool ManagedProcess::WaitForState(bool stopped) {
    // 截止時間
    const Clock::time_point deadline = Clock::now() + kStateChangeTimeout;
    // 在截止時間前反覆檢查
    while (Clock::now() < deadline) {
        // 目前的狀態字母
        const char s = KernelState();
        // 行程已經不存在（0）或已結束等待回收（Z = 殭屍）
        if (s == 0 || s == 'Z') return false;
        // 'T' = 暫停中；符合期望就成功
        if ((s == 'T') == stopped) return true;
        // 睡一下再檢查
        std::this_thread::sleep_for(kStateCheckInterval);
    }
    // 超時
    return false;
}

// 暫停 / 繼續直接用 kill() 送 SIGSTOP / SIGCONT，不用 Boost.Process 的 suspend() / resume()：
// Boost 1.83 有 bug，process::resume() 內部呼叫的是 suspend()；
// 而且在不支援 pidfd 的系統上，suspend 會送 SIGCONT、resume 會送 SIGTERM（剛好相反）。

// 暫停
bool ManagedProcess::Pause() {
    // 已結束的不能暫停
    if (Exited()) { APP_LOG("PROC_NOT_RUNNING", {{"name", name_}}); return false; }
    // 停止中的不能暫停
    if (stopping_) { APP_LOG("PROC_IS_STOPPING", {{"name", name_}}); return false; }
    // 送 SIGSTOP：這個訊號無法被攔截或忽略，行程一定會暫停
    if (::kill(pid_, SIGSTOP) != 0) {
        // Save errno first: building the log strings may overwrite it
        const int err = errno;
        // Log why the signal could not be sent
        APP_LOG("PROC_SIGNAL_FAILED", {{"name", name_}, {"signal", "SIGSTOP"}, {"reason", std::strerror(err)}});
        // 失敗
        return false;
    }
    // 訊號是非同步的，等核心確認已經暫停
    if (!WaitForState(true)) { APP_LOG("PROC_STATE_TIMEOUT", {{"name", name_}, {"action", "pause"}}); return false; }
    // 成功
    return true;
}

// 繼續
bool ManagedProcess::Resume() {
    // 已結束的不能繼續
    if (Exited()) { APP_LOG("PROC_NOT_RUNNING", {{"name", name_}}); return false; }
    // 送 SIGCONT：讓暫停中的行程繼續執行（沒暫停的話沒有影響）
    if (::kill(pid_, SIGCONT) != 0) {
        // Save errno first: building the log strings may overwrite it
        const int err = errno;
        // Log why the signal could not be sent
        APP_LOG("PROC_SIGNAL_FAILED", {{"name", name_}, {"signal", "SIGCONT"}, {"reason", std::strerror(err)}});
        // 失敗
        return false;
    }
    // 等核心確認已經不是暫停狀態
    if (!WaitForState(false)) { APP_LOG("PROC_STATE_TIMEOUT", {{"name", name_}, {"action", "resume"}}); return false; }
    // 成功
    return true;
}

// 停止步驟 1：要求結束
void ManagedProcess::RequestStop() {
    // 已結束就不用做
    if (Exited()) return;
    // 標記為停止中
    stopping_ = true;
    // 暫停中的行程不會處理 SIGTERM，先讓它繼續
    ::kill(pid_, SIGCONT);
    // 錯誤碼（忽略：行程可能剛好自己結束了）
    bp::error_code ec;
    // request_exit() 會送 SIGTERM：程式可以攔截它，做完收尾再結束
    process_->request_exit(ec);
}

// 停止步驟 2：強制結束
void ManagedProcess::Kill() {
    // 已結束就不用做
    if (Exited()) return;
    // 錯誤碼（忽略）
    bp::error_code ec;
    // terminate() 會送 SIGKILL：無法攔截，行程一定會被結束
    process_->terminate(ec);
}

// 停止一個子行程
int ManagedProcess::Stop(std::chrono::milliseconds timeout) {
    // 沒啟動過
    if (!process_) return -1;
    // 送 SIGTERM
    RequestStop();
    // 在 timeout 內結束就回傳結束碼
    if (WaitExit(timeout)) return exit_code_;
    // 還沒結束：送 SIGKILL
    Kill();
    // 等系統回收
    WaitExit(kKillWaitTimeout);
    // 回傳結束碼（被 SIGKILL 是 137）
    return exit_code_;
}

// 取得狀態快照
ProcessInfo ManagedProcess::Info() {
    // 先更新是否已結束
    Poll();
    // 要回傳的快照
    ProcessInfo info;
    // 名字
    info.name = name_;
    // PID
    info.pid = pid_;
    // 結束碼
    info.exit_code = exit_code_;
    // 工作目錄
    info.working_dir = spec_.working_dir;
    // log 路徑
    info.log_path = spec_.log_path;
    // 指令列：執行檔
    info.command = spec_.executable;
    // 指令列：每個參數，用空白隔開
    for (const std::string& a : spec_.args) info.command += " " + a;
    // 沒啟動過：其他欄位保持預設值
    if (!process_) return info;

    // 已執行時間：結束了就算到結束那一刻，否則算到現在
    info.uptime_s = Seconds((exited_ ? exited_at_ : Clock::now()) - started_at_);
    // 已結束
    if (exited_) info.state = ProcessState::Exited;
    // 停止中
    else if (stopping_) info.state = ProcessState::Stopping;
    // 以核心回報為準：'T' = 暫停中（就算是別人用 kill -STOP 暫停的也看得到）
    else if (KernelState() == 'T') info.state = ProcessState::Paused;
    // 其他：執行中
    else info.state = ProcessState::Running;
    // 回傳快照
    return info;
}
