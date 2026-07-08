/**
* mGBA 桥接层 v5 — 适配 mGBA 0.10.5 API
*
* mGBA 0.10.5 头文件中很多便捷函数（如 mCoreRunFrame、mCoreAddKeys 等）
* 实际上是 inline（通过 struct mCore 字段函数指针调用），头文件未声明，
* 也没在 libmgba.so 中导出，dart:ffi 无法直接 lookup。
*
* 本桥接层 wrap 所有这些 inline 函数，提供给 Dart 端调用。
*
* 注意：mGBA 0.10.5 仍导出了部分便捷函数（mCoreLoadConfig、mCoreLoadFile、
* mCoreLoadStateNamed、mCoreSaveStateNamed、mCoreLoadSaveFile 等），
* 这些直接在 Dart 端用 lookupFunction 即可。
*/

#include <mgba/core/core.h>
#include <mgba/core/config.h>
#include <mgba/gba/core.h>
#include <mgba/gba/interface.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/savedata.h>
#include <mgba/internal/gba/memory.h>
#include <mgba-util/vfs.h>
#include <mgba-util/audio-buffer.h>
#include <android/log.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

/* 防止链接器 --gc-sections 把桥接函数删掉 */
#define MGBA_BRIDGE __attribute__((visibility("default"), used))

#define LOG_TAG "MGBA_BRIDGE"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

/* ---- 核心创建/销毁 ---- */

MGBA_BRIDGE struct mCore* mGBACoreCreate(void) {
    struct mCore* core = mCoreCreate(mPLATFORM_GBA);
    if (!core) return NULL;

    /* 必须先调用 init 初始化内部硬件（CPU/内存/视频渲染器/定时器等），
     * 否则 opts 以外的所有结构体成员都是野指针，后续 loadConfig / 运行都会崩溃 */
    if (!core->init(core)) {
        free(core);
        return NULL;
    }

    mCoreInitConfig(core, NULL);
    /* 强制跳过 BIOS：移动设备没有合法 GBA BIOS，跳到 BIOS 地址 → SIGSEGV */
    core->opts.skipBios = true;
    mCoreLoadConfig(core);

    return core;
}

MGBA_BRIDGE void mGBACoreDestroy(struct mCore* core) {
    if (!core) return;
    core->deinit(core);
    /* 不调 free(core)：Android Scudo 环境下 core 堆元数据可能被破坏，
     * free 会触发 SIGABRT。deinit 已清理内部资源，仅泄漏几 KB。 */
}

/* ---- ROM 加载（从内存） ---- */

MGBA_BRIDGE bool mGBALoadROMBytes(struct mCore* core, const void* data, size_t size) {
    if (!core || !data || size == 0) return false;
    LOGI("mGBALoadROMBytes: size=%zu", size);
    /* VFileMemChunk 会复制 ROM 数据到自有缓冲区，Dart 端可以立即释放入参 data。
     * 注意：成功后不能 close vf！GBALoadROM 内部 gba->romVf = vf 持有 VFile 所有权，
     * close 会把 gba->memory.rom 正在使用的内存释放掉。
     *
     * 注意：此处只负责 loadROM，不调 core->reset()。
     * 因为 reset 里 GBAVideoAssociateRenderer 需要 gbacore->renderer.outputBuffer 已设置，
     * 否则会用 dummy renderer（无输出）。Dart 端会在 setVideoBuffer 之后调用 reset。*/
    struct VFile* vf = VFileMemChunk(data, size);
    if (!vf) {
        LOGE("mGBALoadROMBytes: VFileMemChunk failed");
        return false;
    }
    bool ret = core->loadROM(core, vf);
    if (!ret) {
        LOGE("mGBALoadROMBytes: core->loadROM failed");
        vf->close(vf);
        return false;
    }

    /* 打印 ROM 头 0x40 + savedata.type 检测 */
    struct GBA* gba = (struct GBA*) core->board;
    if (gba && gba->memory.rom) {
        char code[5] = {0};
        memcpy(code, gba->memory.rom + 0xAC, 4);
        uint8_t saveType = gba->memory.rom[0x1A];
        LOGI("mGBALoadROMBytes: romCode='%s' saveType=0x%02X", code, saveType);
    } else {
        LOGW("mGBALoadROMBytes: gba->memory.rom is NULL after loadROM");
    }

    return true;
}

/* ---- 运行控制（wrap inline） ---- */

MGBA_BRIDGE void mGBACoreRunFrame(struct mCore* core) {
    if (!core) return;
    /* DEBUG: 关键帧打印 PC+DISPCNT+Flash 状态 */
    static int debugCount = 0;
    core->runFrame(core);
    if (debugCount < 3 || debugCount == 30 || debugCount == 60 ||
        debugCount == 90 || debugCount == 120 || debugCount == 180 || debugCount == 300) {
        struct GBA* gba = (struct GBA*) core->board;
        struct ARMCore* cpu = (struct ARMCore*) core->cpu;
        /* io[0] = DISPCNT, io[3] = VCOUNT */
        LOGI("runFrame[%d] pc=0x%08X dispcnt=0x%04X ly=0x%02X flashType=%d bank=%p data=%p",
             debugCount, cpu->gprs[15],
             gba->memory.io[0],
             gba->memory.io[3],
             gba->memory.savedata.type,
             (void*)gba->memory.savedata.currentBank,
             (void*)gba->memory.savedata.data);
    }
    debugCount++;
}

MGBA_BRIDGE void mGBACoreRunLoop(struct mCore* core) {
    if (!core) return;
    core->runLoop(core);
}

MGBA_BRIDGE void mGBACoreStep(struct mCore* core) {
    if (!core) return;
    core->step(core);
}

MGBA_BRIDGE void mGBACoreReset(struct mCore* core) {
    if (!core) return;
    struct GBA* gba = (struct GBA*) core->board;
    LOGI("mGBACoreReset: before-reset savedata.type=%d vf=%p data=%p",
         gba->memory.savedata.type,
         (void*)gba->memory.savedata.vf,
         (void*)gba->memory.savedata.data);
    core->reset(core);
    LOGI("mGBACoreReset: after-reset savedata.type=%d vf=%p data=%p realVf=%p cpu=%p dispcnt=0x%04X",
         gba->memory.savedata.type,
         (void*)gba->memory.savedata.vf,
         (void*)gba->memory.savedata.data,
         (void*)gba->memory.savedata.realVf,
         (void*)core->cpu,
         gba->memory.io[0]);
}

/* ---- 视频（wrap inline） ---- */

MGBA_BRIDGE void mGBACoreSetVideoBuffer(struct mCore* core, void* buffer, size_t stride) {
    if (!core) return;
    core->setVideoBuffer(core, (mColor*)buffer, stride);
}

MGBA_BRIDGE unsigned mGBACoreGetVideoWidth(struct mCore* core) {
    if (!core) return 0;
    unsigned w = 0, h = 0;
    core->currentVideoSize(core, &w, &h);
    return w;
}

MGBA_BRIDGE unsigned mGBACoreGetVideoHeight(struct mCore* core) {
    if (!core) return 0;
    unsigned w = 0, h = 0;
    core->currentVideoSize(core, &w, &h);
    return h;
}

/* ---- 音频（wrap inline） ---- */

MGBA_BRIDGE unsigned mGBACoreGetAudioSampleRate(struct mCore* core) {
    if (!core) return 0;
    return core->audioSampleRate(core);
}

/* 从 mGBA 内部 mAudioBuffer 读取 N 个采样到 Dart 端 buffer。
 * 使用 mAudioBufferRead 而非 mAudioBufferDump：
 *   mAudioBufferDump 从固定 offset 读取（始终读位置 0 的旧数据）。
 *   mAudioBufferRead 从当前读指针读取并推进，正确消费音频流。
 * outSamples = 总采样数（左右声道合计），返回实际读取的采样数。 */
MGBA_BRIDGE size_t mGBACoreReadAudio(struct mCore* core, int16_t* out, size_t outSamples) {
    if (!core || !out) return 0;
    struct mAudioBuffer* ab = core->getAudioBuffer(core);
    if (!ab) {
        static int warnOnce = 0;
        if (!warnOnce++) LOGE("mGBACoreReadAudio: getAudioBuffer returned NULL!");
        return 0;
    }
    size_t ret = mAudioBufferRead(ab, out, outSamples);
    static int debugCount = 0;
    if (debugCount < 5 && ret >= 8) {
        LOGI("mGBACoreReadAudio[%d]: read=%zu samples[0..7]=%d,%d,%d,%d,%d,%d,%d,%d",
             debugCount, ret,
             out[0], out[1], out[2], out[3], out[4], out[5], out[6], out[7]);
        debugCount++;
    }
    return ret;
}

/* ---- 输入（wrap inline） ---- */

MGBA_BRIDGE void mGBACoreAddKeys(struct mCore* core, uint32_t keys) {
    if (!core) return;
    core->addKeys(core, keys);
}

MGBA_BRIDGE void mGBACoreClearKeys(struct mCore* core, uint32_t keys) {
    if (!core) return;
    core->clearKeys(core, keys);
}

/* ---- 存档（基于文件路径的 save/load state 和 battery） ---- */

/* 即时存档：保存整个模拟器状态到指定路径 */
MGBA_BRIDGE bool mGBASaveStateToFile(struct mCore* core, const char* path) {
    if (!core || !path) return false;
    ssize_t stateSize = core->stateSize(core);
    LOGI("mGBASaveStateToFile: path=%s stateSize=%zd", path, stateSize);
    struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_WRONLY);
    if (!vf) {
        LOGE("mGBASaveStateToFile: VFileOpen failed for %s (errno=%d)", path, errno);
        return false;
    }
    LOGI("mGBASaveStateToFile: VFileOpen ok, calling mCoreSaveStateNamed...");
    bool ret = mCoreSaveStateNamed(core, vf, 0);
    LOGI("mGBASaveStateToFile: mCoreSaveStateNamed ret=%d", ret);
    bool closeOk = vf->close(vf);
    LOGI("mGBASaveStateToFile: vf->close ret=%d, final ret=%d", closeOk, ret && closeOk);
    return ret && closeOk;
}

/* 即时读档：从指定路径恢复整个模拟器状态 */
MGBA_BRIDGE bool mGBALoadStateFromFile(struct mCore* core, const char* path) {
    if (!core || !path) return false;

    struct stat st;
    if (stat(path, &st) != 0) {
        LOGE("mGBALoadStateFromFile: stat failed for %s (errno=%d)", path, errno);
        return false;
    }
    ssize_t expectedSize = core->stateSize(core);
    LOGI("mGBALoadStateFromFile: path=%s fileSize=%lld stateSize=%zd",
         path, (long long)st.st_size, expectedSize);
    if ((long long)st.st_size < expectedSize) {
        LOGE("mGBALoadStateFromFile: file too small (%lld < %zd), possible version mismatch",
             (long long)st.st_size, expectedSize);
        return false;
    }

    struct VFile* vf = VFileOpen(path, O_RDONLY);
    if (!vf) {
        LOGE("mGBALoadStateFromFile: VFileOpen failed for %s (errno=%d)", path, errno);
        return false;
    }
    LOGI("mGBALoadStateFromFile: VFileOpen ok, calling mCoreLoadStateNamed...");
    bool ret = mCoreLoadStateNamed(core, vf, 0);
    LOGI("mGBALoadStateFromFile: mCoreLoadStateNamed ret=%d", ret);
    vf->close(vf);
    return ret;
}

/* 电池存档：保存 SRAM 到指定路径 */
MGBA_BRIDGE bool mGBASaveBatteryToFile(struct mCore* core, const char* path) {
    if (!core || !path) return false;

    /* 先通过 savedataClone 获取 SRAM 缓冲区 */
    void* data = NULL;
    size_t size = core->savedataClone(core, &data);
    if (!data || size == 0) {
        /* 没有存档数据是正常的（游戏还未产生存档） */
        return false;
    }

    struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_WRONLY);
    if (!vf) {
        free(data);
        return false;
    }
    vf->write(vf, data, size);
    vf->close(vf);
    free(data);
    return true;
}

/* 电池读档：从指定路径加载 SRAM。
 * 注意：core->loadSave → GBALoadSave → GBASavedataInit 会把 VFile 存入
 * savedata→vf/realVf，之后 InitFlash 通过 vf→map() 映射存档数据。
 * 此时 VFile 所有权已转移给 savedata，绝不能 close。 */
MGBA_BRIDGE bool mGBALoadBatteryFromFile(struct mCore* core, const char* path) {
    if (!core || !path) return false;

    /* 打印文件大小 */
    struct stat st;
    if (stat(path, &st) != 0) {
        LOGE("mGBALoadBatteryFromFile: stat failed for %s", path);
        return false;
    }
    LOGI("mGBALoadBatteryFromFile: path=%s size=%lld", path, (long long)st.st_size);

    struct GBA* gba = (struct GBA*) core->board;
    LOGI("mGBALoadBatteryFromFile: before-loadSave type=%d vf=%p data=%p",
         gba->memory.savedata.type,
         (void*)gba->memory.savedata.vf,
         (void*)gba->memory.savedata.data);

    /* 必须用 O_RDWR：GBASavedataInitFlash 内部 vf->map() 需要 PROT_WRITE|MAP_SHARED，
     * mmap(PROT_WRITE) 对只读 fd 会返回 EACCES → data=NULL → Flash 无法初始化。 */
    struct VFile* vf = VFileOpen(path, O_RDWR);
    if (!vf) {
        LOGE("mGBALoadBatteryFromFile: VFileOpen failed");
        return false;
    }
    bool ret = core->loadSave(core, vf);
    /* 不要 vf->close(vf)！VFile 所有权已转移给 savedata */
    LOGI("mGBALoadBatteryFromFile: loadSave ret=%d after type=%d vf=%p data=%p realVf=%p",
         ret, gba->memory.savedata.type,
         (void*)gba->memory.savedata.vf,
         (void*)gba->memory.savedata.data,
         (void*)gba->memory.savedata.realVf);
    return ret;
}
