// policy_gen.cpp
// Legacy demo kept in repo (not compiled by default).
// Updated for merged per-APP header generation (inc/<APP>.h).

#include "inc/CONTROLER.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

using namespace std;

static std::string now_text()
{
    std::time_t t = std::time(nullptr);
    std::tm tm {};
    localtime_r(&t, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%F %T");
    return oss.str();
}

static zrtdb_app_controler_ctx_t g_ctx;
static zrtdb_control_controlptr_t* controlptr_;
static zrtdb_control_control_t* control_;
static zrtdb_model_modelptr_t* modelptr_;
static zrtdb_model_model_t* model_;

void DB_init()
{
    if (zrtdb_app_controler_init(&g_ctx) < 0) {
        std::cerr << "zrtdb_app_controler_init failed\n";
        std::exit(1);
    }
    controlptr_ = g_ctx.CONTROL_CONTROLPTR;
    control_    = g_ctx.CONTROL_CONTROL;
    modelptr_   = g_ctx.MODEL_MODELPTR;
    model_      = g_ctx.MODEL_MODEL;
}

int main(int argc, char** argv)
{
    DB_init();

    std::int64_t cmdCounter = 0;

    int MX_COMMANDS = ZRTDB_CONTROL_MX_COMMANDS;

    while (true) {
        ++cmdCounter;
        const int row = static_cast<int>((cmdCounter - 1) % MX_COMMANDS);

        string now_ = now_text();

        SnapshotReadLock_(); // 进入写入批次（协作式）

        controlptr_->STATUS_COMMANDS[row] = 0;
        controlptr_->VAL_COMMANDS[row] = 3.1415926535;
        controlptr_->SIG_COMMANDS[row] = 100;
        controlptr_->ID_COMMANDS[row] = 200;

        string info = now_ + "发出控制指令";

        std::strncpy(controlptr_->INFO_COMMANDS[row], info.c_str(), 120);

        std::cout << "publish: " << cmdCounter << "\n";

        std::atomic_thread_fence(std::memory_order_release); // 添加写屏障
        (*(volatile decltype(control_->LV_COMMANDS)*)&(control_->LV_COMMANDS)) = row + 1;
        // 用 volatile 转换一下，确保编译器立刻生成写内存指令，而不是暂存寄存器
        SnapshotReadUnlock_(); // 退出写入批次

        if (cmdCounter == 6) {
            cout << "测试快照的写入 COMMANDS = " << control_->LV_COMMANDS << endl;
            char path[512];
            SaveSnapshot_(path, sizeof(path));
            std::cout << "snapshot=" << path << "\n";
        }
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
}

// 快照的写入:
// char path[512];
// SaveSnapshot_(path, sizeof(path));
// std::cout << "snapshot=" << path << "\n";

// 快照的读取:
// LoadSnapshot_("CONTROLER_20260202-193012.123");
