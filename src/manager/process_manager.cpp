// 這個檔案實作的類別宣告
#include "process_manager.hpp"

// std::max
#include <algorithm>
// std::isalnum
#include <cctype>
// std::filesystem：建立 log 資料夾、組合 log 路徑
#include <filesystem>
// std::move
#include <utility>

// APP_LOG: write a line from configs/log_messages.yaml
#include "app_log.hpp"

// 匿名命名空間：只在這個檔案內使用
namespace {

// 檢查名字：1 ~ kMaxNameLength 個英文字母、數字、'_' 或 '-'（名字也會用在 log 檔名，所以要限制）
bool IsValidName(const std::string& name) {
    // 空字串或太長
    if (name.empty() || name.size() > kMaxNameLength) return false;
    // 逐一檢查每個字元（轉成 unsigned char，isalnum 才不會對負值出錯）
    for (unsigned char c : name) {
        // 不是英數字、也不是 '_' 或 '-'
        if (!std::isalnum(c) && c != '_' && c != '-') return false;
    }
    // 全部合法
    return true;
}

}  // namespace

// 建構子：只記下 log 資料夾
ProcessManager::ProcessManager(std::string log_dir) : log_dir_(std::move(log_dir)) {}

// 解構子：停止所有子行程
ProcessManager::~ProcessManager() { StopAll(); }

// 用名字找子行程
ManagedProcess* ProcessManager::Find(const std::string& name) {
    // 在 map 裡找
    std::map<std::string, std::unique_ptr<ManagedProcess>>::iterator it = children_.find(name);
    // 找不到
    if (it == children_.end()) {
        // Log the reason
        APP_LOG("MGR_NOT_FOUND", {{"name", name}});
        // 回傳空指標
        return nullptr;
    }
    // 回傳子行程（unique_ptr 裡的原始指標；擁有權仍在 map）
    return it->second.get();
}

// 啟動
bool ProcessManager::Start(const std::string& name, ProcessSpec spec) {
    // 檢查名字
    if (!IsValidName(name)) {
        // Log the reason
        APP_LOG("MGR_INVALID_NAME", {{"name", name}, {"max", std::to_string(kMaxNameLength)}});
        // 失敗
        return false;
    }
    // 是否已經有同名的子行程
    std::map<std::string, std::unique_ptr<ManagedProcess>>::iterator it = children_.find(name);
    // 有同名的
    if (it != children_.end()) {
        // 還在執行：不能重複
        if (!it->second->Exited()) {
            // Log the reason
            APP_LOG("MGR_ALREADY_RUNNING", {{"name", name}});
            // 失敗
            return false;
        }
        // 已結束：移除舊的，讓新的取代它
        children_.erase(it);
    }

    // 沒指定 log 檔，而且有設定 log 資料夾
    if (spec.log_path.empty() && !log_dir_.empty()) {
        // 錯誤碼（不丟例外的版本）
        std::error_code ec;
        // 確保 log 資料夾存在（已存在不會出錯）
        std::filesystem::create_directories(log_dir_, ec);
        // log 檔：<log_dir>/<name>.log
        spec.log_path = (std::filesystem::absolute(log_dir_, ec) / (name + ".log")).string();
    }

    // 建立新的子行程物件（共用同一個 io_context）
    std::unique_ptr<ManagedProcess> child = std::make_unique<ManagedProcess>(context_, name);
    // 啟動；失敗的話 child 會在離開函式時自動釋放
    if (!child->Start(spec)) return false;
    // Log the new process: name, PID, log file
    APP_LOG("MGR_STARTED", {{"name", name}, {"pid", std::to_string(child->Info().pid)}, {"log", spec.log_path}});
    // 加入 map（擁有權移到 map）
    children_.emplace(name, std::move(child));
    // 成功
    return true;
}

// 暫停
bool ProcessManager::Pause(const std::string& name) {
    // 找子行程（Find logs MGR_NOT_FOUND）
    ManagedProcess* child = Find(name);
    // 找不到，或暫停失敗（ManagedProcess logs the reason）
    if (!child || !child->Pause()) return false;
    // Log the success
    APP_LOG("MGR_PAUSED", {{"name", name}});
    // 成功
    return true;
}

// 繼續
bool ProcessManager::Resume(const std::string& name) {
    // 找子行程（Find logs MGR_NOT_FOUND）
    ManagedProcess* child = Find(name);
    // 找不到，或繼續失敗（ManagedProcess logs the reason）
    if (!child || !child->Resume()) return false;
    // Log the success
    APP_LOG("MGR_RESUMED", {{"name", name}});
    // 成功
    return true;
}

// 停止
bool ProcessManager::Stop(const std::string& name, int& exit_code) {
    // 找子行程（Find logs MGR_NOT_FOUND）
    ManagedProcess* child = Find(name);
    // 找不到
    if (!child) return false;
    // Log that stopping has begun (it can take up to kDefaultStopTimeout)
    APP_LOG("MGR_STOPPING", {{"name", name}});
    // 1. SIGTERM（暫停中的會先 SIGCONT）
    child->RequestStop();
    // 2. 等它自己結束；超時就 SIGKILL（logs MGR_KILLED）
    if (!child->WaitExit(kDefaultStopTimeout)) KillRemaining({child}, kDefaultStopTimeout);
    // 結束碼（0 = 正常結束，143 = SIGTERM，137 = SIGKILL）
    exit_code = child->Info().exit_code;
    // Log the exit code
    APP_LOG("MGR_EXITED", {{"name", name}, {"code", std::to_string(exit_code)}});
    // 成功
    return true;
}

// SIGKILL the ones that are still running, then wait for the system to reap them
void ProcessManager::KillRemaining(const std::vector<ManagedProcess*>& list, std::chrono::milliseconds waited) {
    // Each candidate
    for (ManagedProcess* child : list) {
        // Already exited after the SIGTERM: nothing to do
        if (child->Exited()) continue;
        // Log that it had to be killed
        APP_LOG("MGR_KILLED", {{"name", child->Name()}, {"timeout_ms", std::to_string(waited.count())}});
        // SIGKILL
        child->Kill();
        // Wait until it is reaped
        child->WaitExit(kKillWaitTimeout);
    }
}

// 等所有子行程結束
bool ProcessManager::WaitAll(std::chrono::milliseconds timeout) {
    // 所有子行程共用同一個截止時間（不是每個各等 timeout）
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + timeout;
    // 是否全部都結束了
    bool all_exited = true;
    // 逐一等待
    for (auto& [name, child] : children_) {
        // 距離截止時間還剩多久
        const std::chrono::milliseconds remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        // 已經超過就用 0（只檢查一次，不等待）
        const std::chrono::milliseconds left = std::max(remaining, std::chrono::milliseconds(0));
        // 這個還沒結束
        if (!child->WaitExit(left)) all_exited = false;
    }
    // 回傳結果
    return all_exited;
}

// 同時停止全部
void ProcessManager::StopAll(std::chrono::milliseconds timeout) {
    // The children that are still running (only these are stopped and logged)
    std::vector<ManagedProcess*> running;
    // Collect them
    for (auto& [name, child] : children_) {
        // Skip the ones that have already exited
        if (!child->Exited()) running.push_back(child.get());
    }
    // Nothing to stop (e.g. the destructor after 'stop all'): stay silent
    if (running.empty()) return;
    // Log how many are being stopped
    APP_LOG("MGR_STOPPING_ALL", {{"count", std::to_string(running.size())}});
    // 1. 同時對所有子行程送 SIGTERM（暫停中的會先 SIGCONT）
    for (ManagedProcess* child : running) child->RequestStop();
    // 2. 在 timeout 內全部結束；3. 還沒結束的送 SIGKILL（logs MGR_KILLED）
    if (!WaitAll(timeout)) KillRemaining(running, timeout);
    // 4. Log every exit code
    for (ManagedProcess* child : running) {
        // name + exit code
        APP_LOG("MGR_EXITED", {{"name", child->Name()}, {"code", std::to_string(child->Info().exit_code)}});
    }
}

// 移除已結束的子行程
bool ProcessManager::Remove(const std::string& name) {
    // 找子行程（Find logs MGR_NOT_FOUND）
    ManagedProcess* child = Find(name);
    // 找不到
    if (!child) return false;
    // 還在執行：不能移除
    if (!child->Exited()) {
        // Log the reason
        APP_LOG("MGR_STILL_RUNNING", {{"name", name}});
        // 失敗
        return false;
    }
    // 從 map 移除（unique_ptr 會釋放物件）
    children_.erase(name);
    // Log the removal
    APP_LOG("MGR_REMOVED", {{"name", name}});
    // 成功
    return true;
}

// 查詢一個
bool ProcessManager::Status(const std::string& name, ProcessInfo& info) {
    // 找子行程（Find logs MGR_NOT_FOUND）
    ManagedProcess* child = Find(name);
    // 找不到
    if (!child) return false;
    // 取得狀態快照
    info = child->Info();
    // 成功
    return true;
}

// 查詢全部
std::vector<ProcessInfo> ProcessManager::Status() {
    // 結果
    std::vector<ProcessInfo> all;
    // 逐一取得狀態快照（map 依名字排序，所以結果也依名字排序）
    for (auto& [name, child] : children_) all.push_back(child->Info());
    // 回傳
    return all;
}

// 所有名字
std::vector<std::string> ProcessManager::Names() const {
    // 結果
    std::vector<std::string> names;
    // 逐一加入名字
    for (const auto& [name, child] : children_) names.push_back(name);
    // 回傳
    return names;
}
