// 防止這個標頭檔在同一個 .cpp 裡被 include 兩次
#pragma once

// std::chrono::milliseconds
#include <chrono>
// std::map：名字 → 子行程
#include <map>
// std::unique_ptr
#include <memory>
// std::string
#include <string>
// std::vector
#include <vector>

// boost::asio::io_context：所有子行程共用一個
#include <boost/asio/io_context.hpp>

// ManagedProcess、ProcessSpec、ProcessInfo、kDefaultStopTimeout
#include "managed_process.hpp"

// 子行程名字的最大長度（名字也會用在 log 檔名）
inline constexpr std::size_t kMaxNameLength = 32;

// 管理任意多個「有名字」的子行程，種類不限。不是執行緒安全的：要在同一個執行緒呼叫。
//
//     ProcessManager manager("/path/to/logs");
//     ProcessSpec spec;  spec.executable = "/path/to/so_runner";  spec.args = {"/path/to/lib.so"};
//     manager.Start("op1", spec);             // 啟動
//     manager.Pause("op1");                   // 暫停
//     manager.Resume("op1");                  // 繼續
//     manager.Stop("op1", exit_code);         // 停止
//
// Every result (success or failure) is logged with APP_LOG (MGR_* / PROC_* in configs/log_messages.yaml);
// the functions only return true / false.
//     for (const ProcessInfo& p : manager.Status()) { ... }   // 狀態
class ProcessManager {
public:
    // log_dir：每個子行程的輸出寫到 <log_dir>/<名字>.log（"" = 丟棄輸出）
    explicit ProcessManager(std::string log_dir);
    // 解構時停止所有子行程
    ~ProcessManager();

    // 啟動一個子行程並取名 name。
    // name：1~32 個 [A-Za-z0-9_-]，不能和還在執行的子行程重複；同名但已結束的會被取代。
    // spec.log_path 是空的話，會自動設成 <log_dir>/<name>.log。
    bool Start(const std::string& name, ProcessSpec spec);
    // 暫停
    bool Pause(const std::string& name);
    // 繼續
    bool Resume(const std::string& name);
    // 停止；exit_code 是子行程的結束碼
    bool Stop(const std::string& name, int& exit_code);
    // 同時停止全部：一起送 SIGTERM，共用一個 timeout，之後還沒結束的一起送 SIGKILL
    void StopAll(std::chrono::milliseconds timeout = kDefaultStopTimeout);
    // 從清單移除一個已結束的子行程
    bool Remove(const std::string& name);

    // 查詢一個子行程的狀態
    bool Status(const std::string& name, ProcessInfo& info);
    // 查詢所有子行程的狀態（依名字排序）
    std::vector<ProcessInfo> Status();
    // 所有子行程的名字（依名字排序）
    std::vector<std::string> Names() const;

private:
    // 用名字找子行程；找不到回傳 nullptr（and logs MGR_NOT_FOUND）
    ManagedProcess* Find(const std::string& name);
    // SIGKILL every child in list that is still running after the SIGTERM, logging MGR_KILLED for each
    void KillRemaining(const std::vector<ManagedProcess*>& list, std::chrono::milliseconds waited);
    // 等所有子行程結束，最多 timeout；全部結束回傳 true
    bool WaitAll(std::chrono::milliseconds timeout);

    // log 資料夾
    std::string log_dir_;
    // 所有子行程共用的 io_context。必須宣告在 children_ 前面：
    // C++ 依宣告的相反順序解構成員，這樣 children_ 會先解構，io_context 後解構（子行程用到它）
    boost::asio::io_context context_;
    // 名字 → 子行程；std::map 會依名字排序
    std::map<std::string, std::unique_ptr<ManagedProcess>> children_;
};
