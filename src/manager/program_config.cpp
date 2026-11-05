// 這個檔案實作的函式宣告
#include "program_config.hpp"

// std::filesystem：路徑處理
#include <filesystem>
// std::move
#include <utility>

// yaml-cpp：解析 YAML 檔
#include <yaml-cpp/yaml.h>

// APP_LOG: write a line from configs/log_messages.yaml
#include "app_log.hpp"

// 命名空間別名
namespace fs = std::filesystem;

// 匿名命名空間：只在這個檔案內使用
namespace {

  // 把 p 轉成絕對路徑：已經是絕對路徑（或空字串）就不變，相對路徑以 base（YAML 檔的資料夾）為基準
  std::string ResolvePath(const fs::path& base, const std::string& p) {
      // 空字串或絕對路徑：原樣回傳
      if (p.empty() || fs::path(p).is_absolute()) return p;
      // 組合並整理路徑（例如把 "configs/../build" 整理成 "build"）
      return fs::weakly_canonical(base / p).string();
}

}  // namespace

// 讀取程式定義
bool LoadPrograms(const std::string& path, ProgramTable& programs) {
    // YAML 的根節點
    YAML::Node root;
    // 讀檔或語法錯誤時 yaml-cpp 會丟出例外
    try {
        // 讀取並解析整個檔案
        root = YAML::LoadFile(path);
    // 檔案不存在或 YAML 語法錯誤
    } catch (const YAML::Exception& e) {
        // Log the reason
        APP_LOG("CFG_PARSE_FAILED", {{"path", path}, {"reason", e.what()}});
        // 失敗
        return false;
    }

    // 相對路徑的基準：YAML 檔所在的資料夾
    const fs::path base = fs::absolute(path).parent_path();

    // 先讀到暫存表，全部成功才覆蓋 programs（失敗時 programs 保持原樣）
    ProgramTable loaded;
    // 欄位型別不對時 yaml-cpp 會丟出例外
    try {
        // 取得 programs: 節點
        const YAML::Node list = root["programs"];
        // 沒有 programs:，或它不是 map（名字: 內容）
        if (!list || !list.IsMap()) {
            // Log the reason
            APP_LOG("CFG_NO_PROGRAMS", {{"path", path}});
            // 失敗
            return false;
        }
        // 逐一處理每個程式（item.first 是名字，item.second 是內容）
        for (YAML::const_iterator item = list.begin(); item != list.end(); ++item) {
            // 程式名
            const std::string name = item->first.as<std::string>();
            // 程式內容
            const YAML::Node node = item->second;
            // library 是必填
            if (!node["library"]) {
                // Log the reason
                APP_LOG("CFG_NO_LIBRARY", {{"path", path}, {"name", name}});
                // 失敗
                return false;
            }
            // 這個程式的定義
            ProgramDef def;
            // .so 路徑（轉成絕對路徑）
            def.library = ResolvePath(base, node["library"].as<std::string>());
            // 工作目錄（選填，轉成絕對路徑）
            if (node["working_dir"]) def.working_dir = ResolvePath(base, node["working_dir"].as<std::string>());
            // 預設參數（選填，字串陣列）
            if (node["args"]) def.args = node["args"].as<std::vector<std::string>>();
            // 存進暫存表
            loaded[name] = std::move(def);
        }
    // 欄位型別錯誤，例如 args 不是陣列
    } catch (const YAML::Exception& e) {
        // Log the reason
        APP_LOG("CFG_PARSE_FAILED", {{"path", path}, {"reason", e.what()}});
        // 失敗
        return false;
    }
    // 全部成功：覆蓋結果
    programs = std::move(loaded);
    // Log how many programs were loaded
    APP_LOG("CFG_LOADED", {{"count", std::to_string(programs.size())}, {"path", path}});
    // 成功
    return true;
}
