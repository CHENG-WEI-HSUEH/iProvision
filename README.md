# iProvision — 通用行程管理器

用一個命令列程式，同時啟動 **n 個子行程**，每個子行程載入你指定的 **.so 檔**，
並且可以對每個子行程 **啟動 / 暫停 / 繼續 / 停止 / 查看狀態**。

- 原始碼：<https://github.com/CHENG-WEI-HSUEH/iProvision>
- 執行檔：`build/iprovision`
- 測試：GoogleTest，291 個測試案例，含通過率與覆蓋率報告（第 16 節）

```
> start op1 isurgical
> start op2 isurgical --no-rsd
> status
NAME          PID      STATE       UPTIME    COMMAND
op1           232421   running     00:00:09  libisurgical_plugin.so --config configs/isurgical_config....
op2           232422   running     00:00:09  libisurgical_plugin.so --config configs/isurgical_config....
> pause op1
> run op1
> stop all
```

---

## 目錄

1. [架構總覽](#1-架構總覽)
2. [目錄結構](#2-目錄結構)
3. [編譯](#3-編譯)
4. [執行與指令](#4-執行與指令)
5. [configs/processes.yaml](#5-configsprocessesyaml)
6. [一個指令的完整流程](#6-一個指令的完整流程)
7. [狀態、結束碼與訊號](#7-狀態結束碼與訊號)
8. [各模組詳解](#8-各模組詳解)
9. [如何寫一個新的 .so 外掛](#9-如何寫一個新的-so-外掛)
10. [常見修改指南](#10-常見修改指南)
11. [GPU 與 cuDNN（RPATH）](#11-gpu-與-cudnnrpath)
12. [設計決策](#12-設計決策)
13. [疑難排解](#13-疑難排解)
14. [已知限制](#14-已知限制)
15. [Log 系統（glog + log_messages.yaml）](#15-log-系統glog--log_messagesyaml)
16. [測試（GoogleTest + 覆蓋率）](#16-測試googletest--覆蓋率)

---

## 1. 架構總覽

```
                         你在終端機輸入指令
                                │
                                ▼
 ┌───────────────────────────────────────────────────────────────┐
 │ iprovision（管理器，一個行程）                                   │
 │                                                               │
 │   main.cpp ──► Console ──────────► ProcessManager             │
 │               (讀指令、顯示)         │  名字 → ManagedProcess   │
 │                   │                 │  "op1" → ManagedProcess │
 │                   ▼                 │  "op2" → ManagedProcess │
 │           processes.yaml            │  "h1"  → ManagedProcess │
 │       (程式名 → .so、參數、目錄)       └──────────┬──────────────┘
 └──────────────────────────────────────────────────┼────────────┘
                     Boost.Process（fork + exec）     │  SIGSTOP / SIGCONT / SIGTERM / SIGKILL
          ┌──────────────────────┬───────────────────┼──────────────────┐
          ▼                      ▼                   ▼                  │
 ┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐       │
 │ 子行程 op1         │  │ 子行程 op2        │  │ 子行程 h1          │       │
 │ so_runner        │  │ so_runner        │  │ so_runner        │  ...  │
 │  dlopen(.so)     │  │  dlopen(.so)     │  │  dlopen(.so)     │       │
 │  plugin_main()   │  │  plugin_main()   │  │  plugin_main()   │       │
 │      │           │  │      │           │  │      │           │       │
 │ libisurgical_    │  │ libisurgical_    │  │ libtest_         │       │
 │   plugin.so      │  │   plugin.so      │  │   plugin.so      │       │
 │      │           │  │      │           │  └──────────────────┘       │
 │ lib/libisurgical │  │ lib/libisurgical │     stdout/stderr ──► logs/<名字>.log
 │   .so (GPU)      │  │   .so (GPU)      │
 └──────────────────┘  └──────────────────┘
```

### 為什麼需要 so_runner？

- Linux 上「新的行程」一定要從一個**執行檔**開始（`fork` 複製出新行程，再 `exec` 換成另一個執行檔）。
  `.so` 是函式庫，**不能自己執行**。
- Boost.Process 做的就是 `fork + exec`，所以它需要一個執行檔。
- `so_runner` 就是這個執行檔：它只做一件事 —— 用 `dlopen` 載入你指定的 `.so`，
  找到裡面的 `plugin_main()` 並呼叫它。
- 所以「開 n 個子行程跑 .so」= 執行 n 次 `so_runner <某個.so> <參數>`。

### 為什麼需要 libisurgical_plugin.so？

`lib/libisurgical.so` 只提供 C++ 類別（`iSurgical`、`iSurgicalConfig`），
**沒有** `plugin_main()`。`plugins/isurgical_plugin.cpp` 是一層很薄的轉接：
提供 `plugin_main()`，裡面「讀參數 → 建立 `iSurgical` → `Run()`」。

### 每個子行程是完全獨立的

- 各自有自己的記憶體、自己的 GPU context、自己的 log 檔。
- 一個子行程當掉，不會影響管理器或其他子行程。
- 管理器只透過 **訊號**（SIGSTOP 等）控制它們，不共用記憶體。

---

## 2. 目錄結構

```
iProvision/
├── CMakeLists.txt                 編譯設定（逐行註解）
├── README.md                      這份文件
├── configs/
│   ├── processes.yaml             ★ 可以用名字啟動的程式（.so、參數、工作目錄）
│   ├── log_messages.yaml          ★ 所有 log 的 code / level / module / 文字（第 15 節）
│   └── isurgical_config.yaml      isurgical 自己的設定（類別、門檻等）
├── include/
│   ├── plugin_api.hpp             ★ 外掛介面：plugin_main 的定義
│   ├── app_log.hpp                ★ log 介面：APP_LOG(id, {{"key", value}})
│   ├── isurgical.hpp              libisurgical 的公開標頭（外掛用）
│   └── ...                        libisurgical 的其他標頭
├── lib/
│   └── libisurgical.so.0.1.0      預先編譯好的 isurgical 函式庫（+ CMake 建立的 symlink）
├── data/                          isurgical 的模型和影片（相對於 working_dir）；不在 git 裡，見第 3 節
├── logs/                          manager.log（管理器）+ 每個子行程的 <名字>.log（自動建立，不在 git 裡）
├── plugins/
│   ├── isurgical_plugin.cpp       把 libisurgical.so 包裝成外掛
│   └── test_plugin.cpp            測試用外掛（每秒印一行，處理 SIGTERM）
├── src/
│   ├── main.cpp                   進入點：設定訊號、建立 Console、執行
│   ├── app_config.hpp / .cpp      ★ 固定設定：so_runner、processes.yaml、logs 的路徑
│   ├── console/                   命令列介面（跟使用者互動的部分）
│   │   ├── console.hpp / .cpp     Console 類別：主迴圈、每個指令一個函式、顯示
│   │   ├── text_utils.hpp / .cpp  切字、時間格式、打錯字提示
│   │   └── shutdown_signal.hpp / .cpp  Ctrl+C / kill / 關閉終端機的處理
│   ├── log/
│   │   └── app_log.cpp            glog 包裝：讀 log_messages.yaml、組出每一行、送給 glog
│   ├── manager/                   行程管理（不知道 console 的存在，可以單獨拿去用）
│   │   ├── managed_process.hpp / .cpp  ManagedProcess：一個子行程
│   │   ├── process_manager.hpp / .cpp  ProcessManager：名字 → 多個子行程
│   │   └── program_config.hpp / .cpp   讀 processes.yaml
│   └── runner/
│       └── so_runner.cpp          子行程的宿主：dlopen + 呼叫 plugin_main
├── test/                          ★ GoogleTest 測試（獨立的 CMake 專案，第 16 節）
│   ├── CMakeLists.txt             用 --coverage 重新編譯被測程式碼 + 10 個測試執行檔
│   ├── run_tests.sh               編譯 → 執行 → 產生報告（一個指令）
│   ├── common/test_helpers.hpp    共用工具：暫存資料夾、讀寫檔、等待條件、行程狀態
│   ├── fixtures/                  測試專用外掛（exit_code_plugin：印出 argv、回傳指定結束碼）
│   ├── unit/                      單元測試（每個模組一個檔案）
│   ├── integration/               整合測試（Console 完整操作、so_runner + 真實外掛）
│   ├── tools/gen_report.py        GoogleTest XML + gcov → 通過率 / 覆蓋率報告
│   └── reports/                   產生的報告（不在 git 裡）
├── build/                         編譯產物（cmake 產生，不在 git 裡）
    ├── iprovision                 管理器
    ├── so_runner                  子行程宿主
    ├── libapp_log.so              log 函式庫（管理器、so_runner、外掛共用一份）
    ├── libisurgical_plugin.so     isurgical 外掛
    └── libtest_plugin.so          test 外掛
build-test/                        測試的編譯產物（test/run_tests.sh 產生，不在 git 裡）
```

### 分層與依賴方向

```
main.cpp ──► console/ ──► manager/ ──► Boost.Process
   │                          ▲
   └──► app_config            │（只有 ProcessSpec：執行檔 + 參數）
                              │
runner/so_runner ──► include/plugin_api.hpp ◄── plugins/*.cpp
```

- `manager/` **不依賴** `console/`：它是一個通用的「行程管理函式庫」，可以拿到別的程式（例如 GUI、網路服務）使用。
- `manager/` **不知道** `.so` 或 `so_runner` 的存在，它只會執行「某個執行檔 + 參數」。
  「執行 so_runner 並把 .so 當第一個參數」這件事是 `Console::CmdStart` 組出來的。
- `so_runner` 和外掛只透過 `include/plugin_api.hpp` 這一個介面溝通。

---

## 3. 編譯

### 需求

| 套件 | 版本 | 用途 |
|---|---|---|
| CMake | ≥ 3.20 | 建置 |
| g++ | 支援 C++17 | |
| Boost | ≥ 1.80（目前 1.83） | Boost.Process v2、Asio（只用標頭） |
| yaml-cpp | 任何 | 讀 processes.yaml、log_messages.yaml |
| glog | ≥ 0.6（`sudo apt install libgoogle-glog-dev`） | 所有 log |
| OpenCV | 4.x | 只有 isurgical 外掛需要 |
| ONNX Runtime GPU 1.20.1、cuDNN 9.5.1 | | 只有 isurgical 外掛在執行時需要，見第 11 節 |
| GoogleTest | ≥ 1.11（目前 1.14，`sudo apt install libgtest-dev`） | 只有測試需要（第 16 節） |
| gcov、Python 3 | 隨 gcc 附帶 / 任何 3.x | 只有覆蓋率報告需要 |

一次安裝（Ubuntu 24.04）：

```bash
sudo apt install cmake g++ libboost-dev libyaml-cpp-dev libgoogle-glog-dev libopencv-dev libgtest-dev
```

### 取得原始碼

```bash
git clone https://github.com/CHENG-WEI-HSUEH/iProvision.git
cd iProvision
```

`lib/libisurgical.so.0.1.0` 已包含在 repo 裡。**`data/` 不在 repo 裡**（模型和影片太大，GitHub 單檔上限 100 MB；
影片也可能含有病患資料），要執行 isurgical 前請另外放好：

```
data/
├── models/
│   ├── preprocess_step.onnx
│   ├── lc_seg_model.onnx           約 112 MB
│   └── multimodal_rsd_step.onnx    約 389 MB
└── inputs/
    └── video01.mp4                 約 693 MB
```

（預設路徑在 `include/isurgical.hpp` 的 `iSurgicalConfig`，相對於 isurgical 的 `working_dir`（專案根目錄）。
只用 test 外掛或跑測試的話不需要 `data/`。）

### 指令

```bash
cmake -S . -B build              # 產生建置檔（只有第一次或改了 CMakeLists.txt 時需要）
cmake --build build -j$(nproc)   # 編譯全部：管理器、so_runner、兩個外掛
```

- 改了 `.cpp` / `.hpp` 之後：只要再執行第二行。
- 改了 `CMakeLists.txt` 之後：第二行也會自動重新 configure。
- 有奇怪的錯誤時：`rm -rf build` 之後兩行都重跑。

### CMake 不會傳任何值進原始碼

`CMakeLists.txt` 只負責編譯和連結，**沒有** `target_compile_definitions`。
程式裡用到的路徑和設定都寫在原始碼的 `constexpr` / `#define`：

| 設定 | 在哪裡 |
|---|---|
| so_runner、processes.yaml、logs 的路徑 | `src/app_config.hpp` |
| stop 的等待時間、輪詢間隔 | `src/manager/managed_process.hpp` |
| 名字最大長度 | `src/manager/process_manager.hpp` |
| 狀態表欄寬、打錯字距離 | `src/console/console.cpp` 開頭 |
| `BOOST_PROCESS_USE_STD_FS` | `src/manager/managed_process.hpp` 開頭 |
| `PLUGIN_EXPORT`、`PLUGIN_ENTRY_NAME` | `include/plugin_api.hpp` |
| log_messages.yaml 的路徑 | `src/app_config.hpp` 的 `kLogCatalogFile` |
| log 的文字、code、level、時間格式 | **不在程式裡**：`configs/log_messages.yaml` |

---

## 4. 執行與指令

```bash
./build/iprovision        # 從任何資料夾執行都可以
```

### 指令表

| 指令 | 說明 | 範例 |
|---|---|---|
| `start <program> [options...]` | 啟動 processes.yaml 裡的程式，名字 = 程式名 | `start isurgical --show` |
| `start <name> <program> [args...]` | 用自訂名字（nickname）啟動，可以開多個 | `start op1 isurgical` |
| `start <name> <lib.so> [args...]` | 直接載入任何有 `plugin_main` 的 .so | `start x ./build/libtest_plugin.so 0.5` |
| `programs` | 列出 processes.yaml 裡的程式 | |
| `pause <name>\|all` | 暫停（SIGSTOP） | `pause op1`、`pause all` |
| `resume <name>\|all`（別名 `run`、`continue`） | 繼續（SIGCONT） | `run op1` |
| `stop <name>\|all` | 停止：SIGTERM，5 秒後 SIGKILL | `stop op2`、`stop all` |
| `status [name]`（別名 `ls`、`list`） | 全部的狀態表，或一個的詳細資料 | `status`、`status op1` |
| `remove <name>`（別名 `rm`） | 從清單移除已結束的 | `remove op1` |
| `help` / `?` | 說明 | |
| `quit` / `exit` / `q` / Ctrl+D | 停止所有子行程後離開 | |
| Ctrl+C | 同 quit；**再按一次**立刻強制結束 | |

### start 的參數規則

```
start  op1   isurgical   --no-rsd --show
       │     │           └─ 額外參數：接在 processes.yaml 的 args 後面
       │     └─ 第三個字：程式名（processes.yaml）或 .so 路徑
       └─ 第二個字：名字（nickname）

start  isurgical   --show
       │           └─ 第三個字以 '-' 開頭 → 是選項，不是程式；名字 = 程式名
       └─ 程式名
```

- 名字規則：1~32 個英文字母、數字、`_`、`-`。
- 同一時間每個名字只能有一個在執行；已結束的可以用同名重新 `start`（會取代舊的）。
- 參數裡要有空白：用雙引號，例如 `start op1 isurgical --output "my video.mp4"`。
- 程式名打錯會提示：`did you mean 'isurgical'?`

### 子行程的輸出

每個子行程的 stdout 和 stderr 都**附加**到 `logs/<名字>.log`。每次啟動的第一行是 so_runner 寫的 `RUNNER_LOADING`：

```
2026-11-04 18:33:36.124 [info] [5002] [runner] [main] pid 269092: loading /.../build/libtest_plugin.so 1 0.5
2026-11-04 18:33:36.124 [info] [6001] [test_plugin] [plugin_main] test plugin started: pid 269092, interval 1.00 s
```

管理器自己的 log 同時顯示在終端機，並附加到 `logs/manager.log`。格式見第 15 節。

即時查看：`tail -f logs/op1.log`、`tail -f logs/manager.log`

---

## 5. configs/processes.yaml

```yaml
programs:
  isurgical:                                          # 程式名（start 時用）
    library: ../build/libisurgical_plugin.so          # 必填：要載入的 .so
    working_dir: ..                                   # 選填：子行程的工作目錄
    args: ["--config", "configs/isurgical_config.yaml"]   # 選填：預設參數

  test:
    library: ../build/libtest_plugin.so
    args: ["1"]
```

| 欄位 | 必填 | 說明 |
|---|---|---|
| `library` | 是 | `.so` 的路徑。相對路徑以 **這個 yaml 檔所在的資料夾（configs/）** 為基準 |
| `working_dir` | 否 | 子行程的工作目錄（同樣相對於 configs/）。沒填 = 管理器目前所在的目錄。程式裡的相對路徑（例如 isurgical 的 `data/models/...`）會以這裡為基準 |
| `args` | 否 | 預設參數（字串陣列）。`start` 時使用者給的參數會**接在後面** |

- 管理器啟動時讀一次。改了 yaml 要重新啟動管理器（不需要重新編譯）。
- 新增一種程式 = 在這裡加一段（外掛要先編譯好，見第 9 節）。

---

## 6. 一個指令的完整流程

### `start op1 isurgical --no-rsd`

```
Console::Run()                      讀一行 → Tokenize() 切成 ["start","op1","isurgical","--no-rsd"]
 └─ Console::Execute()              第一個字是 "start" → CmdStart()
     └─ Console::CmdStart()
         1. named = true（第三個字 "isurgical" 不是 '-' 開頭）
         2. 在 programs_ 找 "isurgical" → ProgramDef{library, args, working_dir}
         3. args 後面接上 "--no-rsd"
         4. 組 ProcessSpec：
              executable  = /.../build/so_runner
              args        = [/.../libisurgical_plugin.so, --config, configs/..., --no-rsd]
              working_dir = /.../iProvision
         └─ ProcessManager::Start("op1", spec)
             1. IsValidName("op1")
             2. 已有 "op1"？還在跑 → 錯誤；已結束 → 移除
             3. log_path = logs/op1.log
             └─ ManagedProcess::Start(spec)
                 1. 檢查執行檔存在且可執行（access X_OK）
                 2. OpenLog()：開 log（附加）
                 3. new bp::process(...)  ← Boost.Process：fork + exec
                      子行程在 exec 之前執行 DieWithParent::on_exec_setup()：
                        setpgid(0,0)                   自成行程群組
                        prctl(PR_SET_PDEATHSIG, SIGKILL)  管理器死掉時一起死
                      stdin = /dev/null，stdout/stderr = log
                 4. 記下 pid、啟動時間
             4. APP_LOG("MGR_STARTED")       終端機 + logs/manager.log
```
（任何一步失敗：在失敗的地方 `APP_LOG` 記下原因，例如 `MGR_INVALID_NAME`、`PROC_NOT_EXECUTABLE`，然後回傳 false）

接著在**子行程**裡：

```
so_runner /.../libisurgical_plugin.so --config configs/... --no-rsd
 0. app_log::Init(log_messages.yaml, StandardError)   glog 寫到 stderr = logs/op1.log
 1. setvbuf(stdout, 行緩衝)                  log 即時更新；APP_LOG("RUNNER_LOADING")
 2. dlopen(libisurgical_plugin.so)          連帶載入 lib/libisurgical.so、OpenCV、ONNX Runtime...
 3. dlsym("plugin_main")
 4. plugin_main(argc-1, argv+1)              argv[0] = .so 路徑，argv[1..] = 參數
     └─ iSurgicalConfig::FromArgs → iSurgical app(config) → app.Run()
 5. plugin_main 的回傳值 = 子行程的結束碼
```

### `pause op1` / `run op1`

```
Console::CmdPause("op1") → Targets("op1") = ["op1"]（"all" 的話是所有名字）
 └─ ProcessManager::Pause → ManagedProcess::Pause
     1. Exited()？ stopping_？ → 錯誤
     2. kill(pid, SIGSTOP)                 核心立刻凍結整個行程（所有執行緒）
     3. WaitForState(true)                 讀 /proc/<pid>/stat，等狀態變成 'T'
```

`run` 一樣，只是送 `SIGCONT`，等狀態不是 `'T'`。

### `stop op1`

```
ManagedProcess::Stop(5 秒)
 1. RequestStop()：kill(pid, SIGCONT)（暫停中的行程不會處理 SIGTERM）→ request_exit() = SIGTERM
 2. WaitExit(5 秒)：每 50ms 用 waitpid(WNOHANG) 檢查一次
 3. 還沒結束 → Kill() = SIGKILL → WaitExit(2 秒)
 4. 回傳結束碼
```

`stop all` 是 `ProcessManager::StopAll()`：**先對全部**送 SIGTERM，**共用一個** 5 秒期限，
再對還沒結束的**全部**送 SIGKILL。所以停 10 個最多也只要約 7 秒，而不是 70 秒。

### `status`

```
ProcessManager::Status() → 每個 ManagedProcess::Info()
 1. Poll()：waitpid(WNOHANG)，已結束就記下結束碼並回收（避免殭屍行程）
 2. 狀態判斷順序：已結束 → Exited；停止中 → Stopping；/proc 狀態 'T' → Paused；其他 → Running
```

### `quit` / Ctrl+C

```
Ctrl+C → OnSignal() 設 g_shutdown = 1，同時中斷 getline（沒設 SA_RESTART）
 → Console::Run() 離開迴圈 → manager_.StopAll() → return 0
再按一次 Ctrl+C（SA_RESETHAND 已恢復預設）→ 管理器立刻結束 → 子行程因 PDEATHSIG 收到 SIGKILL
```

子行程有自己的行程群組（`setpgid`），所以終端機的 Ctrl+C **不會**直接送到子行程，
一定是管理器來決定怎麼停止它們。

---

## 7. 狀態、結束碼與訊號

### 狀態

| 狀態 | 意思 | 判斷方式 |
|---|---|---|
| `running` | 執行中 | 預設 |
| `paused` | 被暫停 | `/proc/<pid>/stat` 的狀態字母是 `T`（別人用 `kill -STOP` 暫停的也看得到） |
| `stopping` | 已送 SIGTERM，等它結束 | `stopping_` 旗標 |
| `exited(N)` | 已結束，結束碼 N | `waitpid` 回傳了 |

### 結束碼

| 結束碼 | 意思 |
|---|---|
| `0` | 正常結束（`plugin_main` 回傳 0） |
| `1`~`125` | `plugin_main` 回傳的錯誤碼（例如 isurgical 參數錯誤 → 2） |
| `127` | so_runner 載入 .so 失敗（檔案不存在、缺依賴、沒有 plugin_main），原因在 log |
| `143` | 被 SIGTERM 結束（128 + 15）：程式沒有處理 SIGTERM，直接被結束 |
| `137` | 被 SIGKILL 強制結束（128 + 9）：SIGTERM 後 5 秒還沒結束 |
| `139` | 段錯誤 SIGSEGV（128 + 11）：程式當掉 |
| `-1` | 不明（從未啟動，或被別人回收了） |

### 訊號總表

| 訊號 | 誰送給誰 | 什麼時候 | 能攔截嗎 |
|---|---|---|---|
| `SIGSTOP` | 管理器 → 子行程 | `pause` | 不能（一定會暫停） |
| `SIGCONT` | 管理器 → 子行程 | `run`、以及 `stop` 之前 | 可以，但一定會恢復執行 |
| `SIGTERM` | 管理器 → 子行程 | `stop` 第一步 | 可以：程式可以收尾後自己結束（見 test_plugin） |
| `SIGKILL` | 管理器 → 子行程 | `stop` 5 秒後；管理器死掉時（PDEATHSIG） | 不能（一定會結束） |
| `SIGINT` | 終端機 → 管理器 | Ctrl+C | 管理器攔截 → 乾淨地停止全部 |
| `SIGTERM` | 別人 → 管理器 | `kill <管理器pid>` | 管理器攔截 → 同上 |
| `SIGHUP` | 終端機 → 管理器 | 關閉終端機視窗 | 管理器攔截 → 同上 |

---

## 8. 各模組詳解

### `src/app_config.hpp / .cpp`

- 三個 `constexpr` 路徑，都相對於**管理器執行檔所在的資料夾**（build/）：
  `kRunnerFile = "so_runner"`、`kProgramsFile = "../configs/processes.yaml"`、`kLogDir = "../logs"`。
- `ResolveFromExecutable()` 讀 `/proc/self/exe`（Linux 上指向目前執行檔的符號連結）取得 build/ 的位置，
  再組成絕對路徑。所以從哪個資料夾執行都可以，整個專案搬家也不用改。

### `src/console/`（跟使用者互動）

| 檔案 | 內容 |
|---|---|
| `console.hpp/.cpp` | `Console` 類別。`Run()` 是主迴圈；`Execute()` 依第一個字分派到 `CmdStart` / `CmdPause` / `CmdResume` / `CmdStop` / `CmdStatus` / `CmdRemove`；`Print*` 負責顯示。`Targets()` 處理 `all`。 |
| `text_utils.hpp/.cpp` | `Tokenize`（支援雙引號的切字）、`Duration`（秒 → 時:分:秒）、`EditDistance` + `ClosestWord`（Levenshtein 編輯距離，用來提示打錯的程式名） |
| `shutdown_signal.hpp/.cpp` | `InstallShutdownSignals()` 設定 SIGINT/SIGTERM/SIGHUP 的處理；`ShutdownRequested()` 讓主迴圈查詢。訊號處理函式裡只設一個 `volatile sig_atomic_t` 旗標（這是訊號處理函式裡唯一安全的做法） |

### `src/manager/`（通用行程管理，可單獨使用）

**`ManagedProcess`**（一個子行程）

| 成員 | 說明 |
|---|---|
| `Start(spec)` | 找執行檔（沒有 `/` 就在 PATH 找）、轉絕對路徑、開 log、用 `bp::process` 啟動 |
| `Pause()` / `Resume()` | `kill(SIGSTOP/SIGCONT)` + `WaitForState()` 等核心確認 |
| `Stop(timeout)` | `RequestStop()` → `WaitExit(timeout)` → `Kill()` → `WaitExit(2s)` |
| `RequestStop()` / `Kill()` / `WaitExit()` | Stop 的分解步驟，讓 ProcessManager 能同時停止很多個 |
| `Info()` | 狀態快照 `ProcessInfo` |
| `Poll()`（私有） | `waitpid(WNOHANG)`：檢查並回收已結束的子行程，解讀結束碼 |
| `KernelState()`（私有） | 讀 `/proc/<pid>/stat` 的狀態字母 |
| `DieWithParent`（cpp 內） | Boost.Process 初始化器，在子行程 exec 之前執行 `setpgid` 和 `PR_SET_PDEATHSIG` |

**`ProcessManager`**（名字 → 多個子行程）

- 內部是 `std::map<std::string, std::unique_ptr<ManagedProcess>>`，依名字排序。
- 擁有一個所有子行程共用的 `boost::asio::io_context`（Boost.Process 規定要有）。
  **它必須宣告在 `children_` 前面**：C++ 依宣告的相反順序解構，子行程要比 io_context 先解構。
- `StopAll()` 用 `WaitAll()` 讓所有子行程共用一個期限。
- 解構子會呼叫 `StopAll()`，所以管理器正常結束時一定會停止所有子行程。

**`program_config`**：`LoadPrograms()` 用 yaml-cpp 讀 `processes.yaml`，相對路徑轉成絕對路徑。
先讀到暫存表，全部成功才覆蓋，失敗時原本的內容不變。

### `src/runner/so_runner.cpp`

約 40 行：`dlopen(RTLD_NOW | RTLD_LOCAL)` → `dlsym("plugin_main")` → 呼叫。
- `RTLD_NOW`：缺少依賴會在載入時就報錯（而不是執行到一半崩潰）。
- 不呼叫 `dlclose`：外掛的執行緒可能還在用它的程式碼。
- `setvbuf(stdout, _IOLBF)`：輸出到檔案時改成行緩衝，log 才能即時看到。

### `include/plugin_api.hpp`

外掛介面只有一個函式：

```cpp
PLUGIN_EXPORT int plugin_main(int argc, char* argv[]);
```

- `extern "C"`：名字不會被 C++ 改掉，`dlsym("plugin_main")` 才找得到。
- `visibility("default")`：外掛用 `-fvisibility=hidden` 編譯（預設隱藏所有符號），只有這個函式公開。

---

## 9. 如何寫一個新的 .so 外掛

以「my_task」為例：

**步驟 1：寫 `plugins/my_task_plugin.cpp`**

```cpp
#include "app_log.hpp"
#include "plugin_api.hpp"
#include <csignal>

static volatile std::sig_atomic_t g_stop = 0;
static void OnTerm(int) { g_stop = 1; }

PLUGIN_EXPORT int plugin_main(int argc, char* argv[]) {
    std::signal(SIGTERM, OnTerm);          // 建議：處理 SIGTERM，stop 時才能乾淨結束（結束碼 0）
    for (int i = 1; i < argc; ++i) APP_LOG("MYTASK_ARG", {{"index", std::to_string(i)}, {"value", argv[i]}});
    while (!g_stop) {
        // ... 做事 ...
    }
    return 0;                              // 結束碼
}
```

並在 `configs/log_messages.yaml` 的 `messages:` 加上它用到的訊息：

```yaml
  MYTASK_ARG:                     # {index} {value}
    code: 6201
    level: info
    module: my_task_plugin
    description: "arg {index}: {value}"
```

**步驟 2：在 `CMakeLists.txt` 的「外掛」區塊加上**

```cmake
add_library(my_task_plugin SHARED plugins/my_task_plugin.cpp)
target_include_directories(my_task_plugin PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(my_task_plugin PRIVATE app_log)   # 用 APP_LOG 就要連結；有其他依賴也加在這裡
```

並把它加進最後一行：`add_dependencies(iprovision so_runner isurgical_plugin test_plugin my_task_plugin)`

**步驟 3：在 `configs/processes.yaml` 加上**

```yaml
  my_task:
    library: ../build/libmy_task_plugin.so
    args: ["--foo", "1"]
```

**步驟 4：編譯並啟動**

```bash
cmake --build build -j$(nproc)
./build/iprovision
> start t1 my_task
> start t2 my_task --foo 2
```

不想改 yaml 也可以直接：`start t1 ./build/libmy_task_plugin.so --foo 1`。

**外掛的注意事項**

- `plugin_main` 就像 `main`：回傳值是結束碼；可以開執行緒，但回傳前要結束它們。
- 不要在外掛裡呼叫 `exit()` 以外的奇怪結束方式；正常 `return` 就好。
- log 用 `APP_LOG`（不用自己初始化：so_runner 已經做了）。`printf` / `std::cout` 也會寫到 `logs/<名字>.log`，但不會有 timestamp / code 格式。
- 工作目錄是 yaml 的 `working_dir`（沒填就是管理器的目前目錄），相對路徑以它為準。

---

## 10. 常見修改指南

| 想做的事 | 改哪裡 |
|---|---|
| 改 stop 的等待時間（預設 5 秒） | `src/manager/managed_process.hpp` 的 `kDefaultStopTimeout` |
| 改 SIGKILL 後的等待時間 | 同上 `kKillWaitTimeout` |
| 改 processes.yaml / logs / so_runner 的位置 | `src/app_config.hpp` |
| 改名字的長度限制 | `src/manager/process_manager.hpp` 的 `kMaxNameLength` |
| 改名字允許的字元 | `src/manager/process_manager.cpp` 的 `IsValidName()` |
| 改狀態表欄寬 | `src/console/console.cpp` 開頭的 `kNameWidth` 等 |
| 新增一個指令 | ① `console.hpp` 宣告 `void CmdXxx(...)`；② `console.cpp` 的 `Execute()` 加一個 `else if (cmd == "xxx") CmdXxx(name);`；③ 實作 `CmdXxx`；④ `PrintHelp()` 加說明 |
| 指令需要對 `all` 生效 | 在 `CmdXxx` 裡用 `for (const std::string& n : Targets(name))` |
| 狀態表多顯示一欄 | ① `ProcessInfo`（managed_process.hpp）加欄位；② `ManagedProcess::Info()` 填值；③ `Console::PrintTable()` 印出 |
| 讓 start 的參數「取代」yaml 的預設參數（而不是接在後面） | `Console::CmdStart` 裡 `def.args.insert(...)` 那行：改成在有額外參數時先 `def.args.clear()` |
| yaml 新增欄位（例如 `env:`） | ① `ProgramDef` 加欄位；② `LoadPrograms()` 讀它；③ `CmdStart` 把它放進 `ProcessSpec`；④ `ManagedProcess::Start` 使用它（Boost.Process 有 `bp::process_environment`） |
| 在別的程式（GUI、網路服務）裡使用 | 只拿 `src/manager/` 三組檔案，直接用 `ProcessManager`（見 process_manager.hpp 開頭的範例）。注意它不是執行緒安全的 |
| 改某個 log 的文字 / code / level | `configs/log_messages.yaml`，不用重新編譯，重新啟動管理器即可 |
| 新增一個 log | ① `log_messages.yaml` 加一個 id；② 程式裡 `APP_LOG("ID", {{"key", value}})`（記得 `#include "app_log.hpp"`） |
| 只看 warning 以上 | `log_messages.yaml` 的 `settings.min_level: warning` |
| 讓子行程的輸出也顯示在終端機 | `ManagedProcess::Start` 的 `bp::process_stdio`：把 log 換成 `stdout`（但多個子行程的輸出會混在一起） |

---

## 11. GPU 與 cuDNN（RPATH）

這台機器的顯示卡是 **GTX 1080 Ti（Pascal, sm_61）**，限制如下：

| 元件 | 必須用 | 原因 |
|---|---|---|
| ONNX Runtime | `onnxruntime-linux-x64-gpu-1.20.1`（CUDA 12） | 1.30 需要 CUDA 13，CUDA 13 不支援 Pascal |
| cuDNN | 9.5.1（`/media/.../Onnx/cudnn-9.5.1/lib`） | 系統的 cuDNN 9.26 沒有 Pascal kernel，會出現 `CUDNN_STATUS_EXECUTION_FAILED_CUDART` 然後退回 CPU |

### 為什麼 cuDNN 路徑設在 so_runner？

- ONNX Runtime 在執行時用 `dlopen` 載入 `libonnxruntime_providers_cuda.so`，它再載入 cuDNN。
- 這種「執行時才載入的函式庫」要去哪裡找，主要看**執行檔**的 `DT_RPATH`。
- 子行程的執行檔是 `so_runner`，所以 `CMakeLists.txt` 把 `lib/`、cuDNN 9.5.1、ONNX Runtime 三個路徑寫進 `so_runner` 的 RPATH，
  並用 `-Wl,--disable-new-dtags` 確保是 `DT_RPATH`（較新的 `DT_RUNPATH` 不會套用到間接依賴）。
- 確認方式：`readelf -d build/so_runner | grep RPATH`。
- **不要**設定 `LD_LIBRARY_PATH` 指向系統 cuDNN：它比 RPATH 優先，會把問題帶回來。
- 換電腦或換路徑：`cmake -S . -B build -DCUDNN_LIB_DIR=... -DONNXRUNTIME_LIB_DIR=...`。

### 檢查 GPU 是否正常

```bash
grep -iE 'cudnn|falling back' logs/op1.log    # 沒有輸出 = 正常使用 GPU
nvidia-smi                                    # 每個 isurgical 子行程都會佔用一些顯示卡記憶體
```

---

## 12. 設計決策

| 決策 | 原因 |
|---|---|
| 子行程用 `fork + exec so_runner`，不是只 `fork` 然後直接呼叫 .so | 只 `fork` 會複製整個管理器（包括 Asio 的狀態），而 CUDA / ONNX Runtime 在 fork 出來、沒有 exec 的行程裡很容易出問題。exec 讓每個子行程都是乾淨的新程式 |
| 一個子行程 = 一個 .so 實例，而不是一個行程裡跑多個 .so | 行程之間完全隔離：一個當掉不影響其他；暫停 / 停止可以針對單一實例；isurgical 用了全域狀態與 GPU context，同一行程跑兩份會互相干擾 |
| 暫停 / 繼續直接 `kill(SIGSTOP/SIGCONT)` | Boost 1.83 的 `process::resume()` 有 bug（實際呼叫的是 `suspend()`）；fallback 版本的 suspend/resume 送的訊號也相反 |
| `PR_SET_PDEATHSIG` 用 SIGKILL | 暫停中的行程不會處理 SIGTERM，用 SIGTERM 的話管理器當掉時暫停中的子行程會永遠卡住、佔著 GPU 記憶體 |
| 子行程 `setpgid(0,0)` | 讓 Ctrl+C 只送到管理器，由管理器決定怎麼停止子行程 |
| 暫停狀態以 `/proc/<pid>/stat` 為準 | 就算有人在外面用 `kill -STOP/-CONT` 改變狀態，`status` 也是正確的 |
| 單執行緒，不是執行緒安全 | 指令是一個一個處理的，不需要鎖。`PR_SET_PDEATHSIG` 也要求從同一個執行緒啟動子行程 |
| 路徑用 constexpr + `/proc/self/exe`，不用 CMake 傳入 | 設定集中在原始碼裡，一眼看得到；不寫死絕對路徑，專案可以搬家 |
| 不用 lambda | 可讀性：每個動作都是有名字的函式，容易追蹤和除錯 |

---

## 13. 疑難排解

| 現象 | 原因 / 解法 |
|---|---|
| `'xxx' is not a program in .../processes.yaml` | 程式名打錯，或 yaml 裡沒有它。用 `programs` 看有哪些 |
| `exited(127)` | so_runner 載入失敗。看 `logs/<名字>.log` 最後一行：檔案不存在、缺少依賴的 .so、或沒有 `plugin_main` |
| `exited(143)` | 正常的 stop，但程式沒處理 SIGTERM。要乾淨結束就在外掛裡處理 SIGTERM |
| `exited(137)` | SIGTERM 後 5 秒還沒結束，被強制 SIGKILL |
| `exited(2)`（isurgical） | isurgical 的參數錯誤，log 裡有它的用法說明 |
| `'op1' is already running` | 名字重複。換個名字，或先 `stop op1` |
| `[warning] [1002]` 之前有 `[4002]`～`[4004]` | processes.yaml 語法錯誤或缺少 `library`。只能用 `start <name> <lib.so>` |
| `[error] [9001] ... cannot load log catalogue` | log_messages.yaml 語法錯誤或不存在。程式照常執行，但每個 log 都會變成 `[9002] unknown log id`（原本的參數仍會印出） |
| `[error] [9002] unknown log id 'XXX'` | 程式用了 yaml 裡沒有的 id：在 log_messages.yaml 加上它 |
| CMake：`Could not find a package configuration file provided by "glog"` | `sudo apt install libgoogle-glog-dev` |
| log 裡有 `CUDNN_FE failure` / `falling back to CPU` | 載入了系統的 cuDNN。檢查 `readelf -d build/so_runner \| grep RPATH`、`echo $LD_LIBRARY_PATH` |
| `so_runner` 找不到 | 管理器和 so_runner 要在同一個資料夾。重新 `cmake --build build` |
| 管理器被 `kill -9` 之後 | 子行程會因 PDEATHSIG 一起被 SIGKILL，不會殘留。確認：`pgrep -a so_runner` |

---

## 14. 已知限制

- **isurgical 停止時是 143**：`libisurgical.so` 沒有公開的「停止」函式，也不處理 SIGTERM，
  所以 stop 會直接結束它，`--output` 的錄影檔可能沒有正常關閉。
  要改善需要在 isurgical 專案裡加一個停止介面，再讓 `isurgical_plugin.cpp` 在收到 SIGTERM 時呼叫它。
- **暫停影片輸入**：isurgical 把影片當成即時串流播放，暫停後再繼續會跳過暫停期間的畫面。
- **暫停不釋放資源**：暫停中的子行程仍然佔用記憶體和 GPU 記憶體。
- **start 的額外參數是接在預設參數後面**，不會取代（見第 10 節如何改）。
- **改 processes.yaml 要重新啟動管理器**。
- **只支援 Linux**：用到 `/proc`、`prctl`、`dlopen`、POSIX 訊號。
- **libisurgical.so 自己印的東西**（推論進度、ONNX Runtime 的警告）是直接寫到 stdout/stderr 的，不經過 glog，所以沒有 `[level] [code]` 格式。
- **改 log_messages.yaml 要重新啟動管理器**；已經在跑的子行程會繼續用它啟動時讀到的版本。
- **已知缺陷：沒有參數的未知指令**（例如打錯字 `stauts`）會顯示 `missing name - usage: stauts <name>|all`，
  而不是 `unknown command 'stauts'`。原因：`Console::Execute()` 先檢查「沒有名字」，才檢查指令是否存在。
  測試 `ConsoleTest.UnknownCommandWithoutArgumentIsReportedAsUnknown` 會在修好之後通過（第 16 節）。

---

## 15. Log 系統（glog + log_messages.yaml）

### 格式

每一行 log 都是：

```
<timestamp>             [level]  [code] [module]  [function] <description>
2026-11-04 18:33:36.119 [info]   [2001] [manager] [Start]    started 't1' (pid 269092), log: /.../logs/t1.log
2026-11-04 18:33:38.502 [error]  [2004] [manager] [Start]    't1' is already running
2026-11-04 18:33:40.902 [warning][1005] [console] [PrintUnknownProgram] 'isrugical' is not a program in ...
```

| 欄位 | 來源 |
|---|---|
| timestamp | 程式產生；格式由 yaml 的 `settings.timestamp_format`、`settings.milliseconds` 決定 |
| level | yaml 的 `level`（info / warning / error），也決定 glog 的嚴重度和終端機顏色 |
| code | yaml 的 `code` |
| module | yaml 的 `module` |
| function | 自動填入寫這行 log 的 C++ 函式名稱（`__func__`）；yaml 有寫 `function:` 就用 yaml 的 |
| description | yaml 的 `description`，其中的 `{name}`、`{pid}` 等由程式填入 |

### 訊息表 `configs/log_messages.yaml`

```yaml
settings:
  timestamp_format: "%Y-%m-%d %H:%M:%S"   # strftime() 格式
  milliseconds: true                      # 時間後面加 .123
  min_level: info                         # 最低輸出等級：info / warning / error
  console: true                           # 管理器：也顯示在終端機
  color: true                             # 管理器：warning 黃色、error 紅色
  manager_log_file: ../logs/manager.log   # 管理器的 log 檔（"" = 不寫檔）；相對於 configs/

messages:
  MGR_STARTED:                    # ← id：程式裡寫 APP_LOG("MGR_STARTED", ...)
    code: 2001
    level: info
    module: manager
    description: "started '{name}' (pid {pid}), log: {log}"   # 註解列出可以用的佔位符
```

- **可以自由修改**：`code`、`level`、`module`、`description`（文字、順序、語言都可以改，佔位符可以刪掉或重複使用）、`settings`。
- **不要修改**：id（程式靠它找訊息）和佔位符的名字（`{name}` 不能改成 `{nickname}`，程式只會填 `name`）。
- 改完重新啟動管理器即可，**不用重新編譯**。
- code 的分段：1xxx console、2xxx manager、3xxx process、4xxx config、5xxx runner、6xxx 外掛、9xxx log 系統本身。

### 在程式裡寫 log

```cpp
#include "app_log.hpp"

APP_LOG("MGR_PAUSED", {{"name", name}});                                  // 一個佔位符
APP_LOG("MGR_EXITED", {{"name", name}, {"code", std::to_string(code)}});  // 值一定要是字串
APP_LOG("CONSOLE_EXIT");                                                  // 沒有佔位符
```

`APP_LOG` 是巨集，會自動把目前函式名稱（`__func__`）傳給 `app_log::Write()`。

### 資料流

```
APP_LOG("MGR_STARTED", {...})
 └─ app_log::Write(id, __func__, params)          src/log/app_log.cpp
     1. 在訊息表找 id（找不到 → LOG_UNKNOWN_ID / 9002）
     2. Fill()：把 {name} 等換成參數值
     3. Emit()：組出 "timestamp [level] [code] [module] [function] description"
     4. google::LogMessage(..., severity).stream() << line     ← glog
          管理器：  glog → 終端機 stderr（FLAGS_alsologtostderr）
                         → FileSink → logs/manager.log（附加）
          子行程：  glog → stderr（FLAGS_logtostderr）= logs/<名字>.log
```

### 重點設計

| 決策 | 原因 |
|---|---|
| `libapp_log.so` 是共享函式庫 | so_runner 和它 dlopen 的外掛共用同一份訊息表和同一個 glog，外掛不用自己初始化 |
| 關掉 glog 自己的前綴（`FLAGS_log_prefix = false`） | glog 預設前綴是 `I1104 18:33:36.119 1234 file.cc:12]`，不符合要求的格式；我們自己組前綴 |
| manager.log 用自訂的 `google::LogSink`（FileSink），不用 glog 自己的 log 檔 | glog 0.6 的 log 檔一定會在開頭寫 4 行 `Log file created at ...`，破壞「每行都以 timestamp 開頭」；而且 sink 可以用附加模式 |
| `FLAGS_logbuflevel = -1`、`FLAGS_logbufsecs = 0` | glog 預設會把 INFO 緩衝最多 30 秒，這裡改成每行立刻寫出 |
| 錯誤在發生的地方記錄，函式只回傳 true / false | `[module] [function]` 才會是真正出錯的地方（例如 `[process] [Start]`），不需要把錯誤字串一層一層傳回 console |
| 訊息表載入失敗不會讓程式停止 | 用內建的 9001 記錄原因，之後的 log 以 9002 印出 id 和所有參數，資訊不會遺失 |
| 表格、help、`did you mean` 仍用 `std::cout` | 它們是互動介面的畫面，不是事件記錄 |

---

## 16. 測試（GoogleTest + 覆蓋率）

`test/` 是一個**獨立的 CMake 專案**：它用 `--coverage` 把被測程式碼（`src/`、`so_runner`、`libapp_log.so`、
`libtest_plugin.so`）重新編譯一份，主專案的 `CMakeLists.txt` 和 `build/` 完全不受影響。
所有測試程式碼、`test/CMakeLists.txt`、`run_tests.sh`、`gen_report.py` 都是**逐行註解**。

### 執行

```bash
test/run_tests.sh              # 編譯 + 執行全部測試 + 產生報告（約 20 秒）
test/run_tests.sh --no-build   # 不重新編譯，只重跑測試和報告
```

結束碼：全部通過 = 0，有任何失敗 = 1（報告照樣會產生）。也可以用 ctest：

```bash
cmake -S test -B build-test -DCMAKE_BUILD_TYPE=Debug
cmake --build build-test -j$(nproc)
ctest --test-dir build-test --output-on-failure
```

- 測試的編譯資料夾**必須是專案根目錄的直接子資料夾**（`build-test/`）：so_runner 用 `../configs/log_messages.yaml` 找訊息表。
- 測試會啟動真實的子行程（`/bin/sh`、`/bin/sleep`、so_runner + 外掛），暫存檔都放在 `build-test/tmp/`，
  不會寫到專案的 `logs/`。設定 `KEEP_TEST_TMP=1` 可以保留暫存檔來除錯。
- 不需要 GPU、`data/` 或 `lib/libisurgical.so`。

### 報告（`test/reports/`）

| 檔案 | 內容 |
|---|---|
| `test_report.html` | 通過率、行 / 函式 / 分支覆蓋率、每個測試案例的結果、**標色的原始碼**（綠 = 有執行、紅 = 沒執行） |
| `test_report.md` | 同樣的摘要（Markdown） |
| `summary.json` | 數字（給 CI 用） |
| `xml/<suite>.xml` | GoogleTest 原始結果（JUnit 格式） |
| `logs/<suite>.log` | 每個測試執行檔的完整輸出 |

覆蓋率用 gcc 附帶的 `gcov --json-format`，由 `test/tools/gen_report.py`（只用 Python 標準函式庫）整理，
不需要 lcov / gcovr。分支覆蓋率不計入「只在丟出例外時才走的分支」。

### 目前結果

| 指標 | 結果 |
|---|---|
| 測試案例 | 291 個，10 個執行檔 |
| 通過率 | **99.7%**（290 / 291；唯一的失敗是第 14 節的已知缺陷） |
| 行覆蓋率 | **97.5%**（715 / 733） |
| 函式覆蓋率 | 98.9%（89 / 90） |
| 分支覆蓋率 | 74.3%（多數未覆蓋的分支是 gcc 為 `std::string` / iostream 產生的隱含分支） |

沒有計入覆蓋率：`src/main.cpp`（只有組裝，呼叫的每個部分都有直接測試）、
`plugins/isurgical_plugin.cpp`（需要 libisurgical、OpenCV、ONNX Runtime GPU）。

### 測試內容

| 執行檔 | 被測對象 | 主要情境 |
|---|---|---|
| `test_text_utils` | `text_utils.cpp` | 表格驅動（`TEST_P`）：引號 / 空白 / UTF-8 切字、`HH:MM:SS`、Levenshtein、打錯字提示的門檻與平手 |
| `test_app_config` | `app_config.cpp` | 常數值、`ResolveFromExecutable` 的相對 / 絕對 / `..` 路徑 |
| `test_shutdown_signal` | `shutdown_signal.cpp` | 死亡測試（獨立子行程）：SIGINT / SIGTERM / SIGHUP 第一次只設旗標、第二次照預設行為結束 |
| `test_app_log` | `app_log.cpp` | 格式、佔位符、等級、未知 id；死亡測試裡用自訂訊息表測試載入失敗、`min_level` 過濾、manager.log 檔案輸出 |
| `test_program_config` | `program_config.cpp` | 真實的 processes.yaml、各種錯誤 YAML、相對路徑、失敗時原表不變 |
| `test_blocking_queue` | `include/blocking_queue.hpp` | FIFO、丟棄最舊、`Close` 喚醒等待者、多生產者 / 多消費者不遺失 |
| `test_managed_process` | `managed_process.cpp` | 真實子行程：結束碼、log 輸出、工作目錄、暫停 / 繼續、SIGTERM → SIGKILL、管理器死掉時子行程一起結束 |
| `test_process_manager` | `process_manager.cpp` | 名字規則、同名取代、`StopAll` 共用期限、`Remove`、排序 |
| `test_console` | `console.cpp` | 用腳本餵 `std::cin`、擷取 `std::cout`：每個指令與別名、真實 so_runner + libtest_plugin.so 的完整流程 |
| `test_so_runner` | `so_runner.cpp`、`test_plugin.cpp` | 結束碼 2 / 127 / 外掛回傳值、argv 平移、優雅停止、暫停中不輸出 |

### 新增測試

1. 在 `test/unit/` 或 `test/integration/` 新增 `test_xxx.cpp`（`#include "test_helpers.hpp"`，在 fixture 的 `SetUp()` 呼叫 `EnsureLogInit()`）。
2. 在 `test/CMakeLists.txt` 最後加一行：`add_gtest(test_xxx unit/test_xxx.cpp)`。
3. `test/run_tests.sh`：新的執行檔會自動被執行並列入報告。
