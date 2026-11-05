// 防止這個標頭檔在同一個 .cpp 裡被 include 兩次
#pragma once

// 讓 Boost.Process 使用 C++17 的 std::filesystem，而不是 boost::filesystem
// （後者需要額外連結 libboost_filesystem）。必須在 include 任何 Boost.Process 標頭「之前」定義。
// 專案裡所有用到 Boost.Process 的地方都會先 include 這個標頭，所以定義在這裡。
#ifndef BOOST_PROCESS_USE_STD_FS
// 定義這個巨集（不需要值）
#define BOOST_PROCESS_USE_STD_FS
// 結束 #ifndef
#endif

// std::chrono::milliseconds 等時間型別
#include <chrono>
// std::unique_ptr
#include <memory>
// std::string
#include <string>
// std::vector
#include <vector>

// boost::asio::io_context：Boost.Process 的每個 process 物件都需要一個 io_context
#include <boost/asio/io_context.hpp>
// boost::process::v2::process：代表一個子行程
#include <boost/process/v2/process.hpp>

// stop 時：送出 SIGTERM 後，最多等多久讓子行程自己結束，之後送 SIGKILL
inline constexpr std::chrono::milliseconds kDefaultStopTimeout{5000};
// 送出 SIGKILL 後，最多等多久讓系統回收子行程
inline constexpr std::chrono::milliseconds kKillWaitTimeout{2000};
// 等待子行程結束時，每隔多久檢查一次
inline constexpr std::chrono::milliseconds kPollInterval{50};
// 暫停 / 繼續後，最多等多久讓核心回報狀態已改變
inline constexpr std::chrono::milliseconds kStateChangeTimeout{500};
// 等待狀態改變時，每隔多久檢查一次
inline constexpr std::chrono::milliseconds kStateCheckInterval{5};

// 子行程的狀態
enum class ProcessState {
    // 正在執行
    Running,
    // 被 SIGSTOP 暫停（不管是管理器還是別人暫停的）
    Paused,
    // 已經要求停止（送了 SIGTERM），正在等它結束
    Stopping,
    // 已結束；結束碼在 ProcessInfo::exit_code
    Exited,
};

// 把狀態轉成文字，例如 ProcessState::Paused → "paused"
const char* ToString(ProcessState state);

// 要執行什麼：啟動子行程時的參數
struct ProcessSpec {
    // 執行檔：路徑，或在 PATH 裡找的名字（例如 "ping"）
    std::string executable;
    // 傳給執行檔的參數（不含執行檔本身）
    std::vector<std::string> args;
    // 子行程的工作目錄；"" = 管理器目前的目錄
    std::string working_dir;
    // stdout + stderr 附加寫入的檔案；"" = 丟棄輸出
    std::string log_path;
};

// 某個子行程在某個時間點的狀態快照（由 ManagedProcess::Info() 產生）
struct ProcessInfo {
    // 子行程的名字（nickname）
    std::string name;
    // 作業系統給的行程編號；還沒啟動是 -1
    int pid = -1;
    // 目前狀態
    ProcessState state = ProcessState::Exited;
    // 已結束時的結束碼；被訊號結束的話是 128 + 訊號編號（例如 SIGTERM=15 → 143）
    int exit_code = -1;
    // 已經執行了幾秒（結束後就停在結束的那一刻）
    double uptime_s = 0;
    // 完整指令列：執行檔 + 參數
    std::string command;
    // 子行程的工作目錄
    std::string working_dir;
    // log 檔的路徑
    std::string log_path;
};

// 一個子行程：用 Boost.Process v2 啟動，可以暫停、繼續、停止、查詢狀態。適用於任何執行檔。
// 不是執行緒安全的：所有成員函式都要在同一個執行緒呼叫。
class ManagedProcess {
public:
    // context：Boost.Process 需要的 io_context（由 ProcessManager 擁有）；name：子行程的名字
    ManagedProcess(boost::asio::io_context& context, std::string name);
    // 解構時如果子行程還在執行，會先停止它
    ~ManagedProcess();
    // 禁止複製：一個子行程只能有一個擁有者
    ManagedProcess(const ManagedProcess&) = delete;
    // 禁止複製指派
    ManagedProcess& operator=(const ManagedProcess&) = delete;

    // 啟動子行程；失敗回傳 false（the reason is logged: PROC_* in configs/log_messages.yaml）
    bool Start(const ProcessSpec& spec);
    // 暫停（送 SIGSTOP），並等到核心確認已暫停；失敗時記錄原因並回傳 false
    bool Pause();
    // 繼續（送 SIGCONT），並等到核心確認已恢復；失敗時記錄原因並回傳 false
    bool Resume();

    // 停止：先 SIGTERM（暫停中的話先 SIGCONT），等 timeout，還沒結束就 SIGKILL。回傳結束碼
    int Stop(std::chrono::milliseconds timeout = kDefaultStopTimeout);

    // 以下三個是 Stop() 的分解步驟，讓 ProcessManager 可以「同時」停止很多個子行程
    // 步驟 1：暫停中的話先 SIGCONT，然後送 SIGTERM（不等待）
    void RequestStop();
    // 步驟 2（如果需要）：送 SIGKILL，強制結束
    void Kill();
    // 等子行程結束，最多 timeout；已結束回傳 true
    bool WaitExit(std::chrono::milliseconds timeout);

    // 子行程是否已經結束（或從未啟動）
    bool Exited();
    // 取得目前狀態的快照
    ProcessInfo Info();
    // 子行程的名字
    const std::string& Name() const { return name_; }

private:
    // 檢查子行程是否已結束；結束的話記下結束碼並回收（避免殭屍行程）。還在執行回傳 true
    bool Poll();
    // 從 /proc/<pid>/stat 讀出核心回報的狀態字母（R 執行、S 睡眠、T 暫停、Z 殭屍...）；讀不到回傳 0
    char KernelState() const;
    // 等到核心回報「已暫停」（stopped=true）或「未暫停」（stopped=false），最多 kStateChangeTimeout
    bool WaitForState(bool stopped);

    // Boost.Process 需要的 io_context（ProcessManager 擁有，壽命比這個物件長）
    boost::asio::io_context& context_;
    // 子行程的名字
    std::string name_;
    // 啟動時的設定（executable 和 working_dir 已轉成絕對路徑）
    ProcessSpec spec_;
    // Boost.Process 的子行程物件；還沒啟動時是空的
    std::unique_ptr<boost::process::v2::process> process_;
    // 子行程的 PID
    int pid_ = -1;
    // 是否已經要求停止
    bool stopping_ = false;
    // 是否已經結束並回收
    bool exited_ = false;
    // 結束碼
    int exit_code_ = -1;
    // 啟動的時間點（用來算 uptime）
    std::chrono::steady_clock::time_point started_at_;
    // 結束的時間點
    std::chrono::steady_clock::time_point exited_at_;
};
