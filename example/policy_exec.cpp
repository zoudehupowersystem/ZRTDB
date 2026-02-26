// policy_exec.cpp
// Legacy demo kept in repo (not compiled by default).
// Updated for merged per-APP header generation (inc/<APP>.h).

#include "inc/CONTROLER.h"

#include <atomic>
#include <chrono>
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

    // 测试快照的下装
    // LoadSnapshot_("CONTROLER_20260202-200637.932");
    // cout << " COMMANDS = " << control_->LV_COMMANDS << endl;

    int COMMANDS_nums { 0 };

    while (true) {
        int COMMANDS_nums_now { 0 };
        {
            COMMANDS_nums_now = *(volatile decltype(control_->LV_COMMANDS)*)&(control_->LV_COMMANDS);
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        if (COMMANDS_nums_now != COMMANDS_nums) {
            COMMANDS_nums = COMMANDS_nums_now;
            for (int i = 0; i < COMMANDS_nums; ++i) {
                if (controlptr_->STATUS_COMMANDS[i] == 2) {
                    continue;
                }
                string msg = std::string(reinterpret_cast<const char*>(controlptr_->INFO_COMMANDS[i]), 120);
                msg += "VAL_COMMANDS= ";
                msg += to_string(controlptr_->VAL_COMMANDS[i]);
                msg += "SIG_COMMANDS= ";
                msg += to_string(controlptr_->SIG_COMMANDS[i]);
                msg += "ID_COMMANDS= ";
                msg += to_string(controlptr_->ID_COMMANDS[i]);
                msg += " 控制执行";
                controlptr_->STATUS_COMMANDS[i] = 2; // 执行完毕更新标志位
                cout << msg << endl;
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
