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
#include <mgba-util/vfs.h>
#include <mgba-util/audio-buffer.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>

/* 防止链接器 --gc-sections 把桥接函数删掉 */
#define MGBA_BRIDGE __attribute__((visibility("default"), used))

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
    free(core);
}

/* ---- ROM 加载（从内存） ---- */

MGBA_BRIDGE bool mGBALoadROMBytes(struct mCore* core, const void* data, size_t size) {
    if (!core || !data || size == 0) return false;
    /* VFileMemChunk 会复制 ROM 数据到自有缓冲区，Dart 端可以立即释放入参 data。
     * 注意：成功后不能 close vf！GBALoadROM 内部 gba->romVf = vf 持有 VFile 所有权，
     * close 会把 gba->memory.rom 正在使用的内存释放掉。
     *
     * 注意：此处只负责 loadROM，不调 core->reset()。
     * 因为 reset 里 GBAVideoAssociateRenderer 需要 gbacore->renderer.outputBuffer 已设置，
     * 否则会用 dummy renderer（无输出）。Dart 端会在 setVideoBuffer 之后调用 reset。*/
    struct VFile* vf = VFileMemChunk(data, size);
    if (!vf) return false;
    bool ret = core->loadROM(core, vf);
    if (!ret) {
        vf->close(vf);
    }
    return ret;
}

/* ---- 运行控制（wrap inline） ---- */

MGBA_BRIDGE void mGBACoreRunFrame(struct mCore* core) {
    if (!core) return;
    core->runFrame(core);
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
    core->reset(core);
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
 * channels 默认为 2（立体声），实际采样数 = available / channels。
 * 返回实际写入的采样数（每采样 1 个 int16，多声道交错存放）。 */
MGBA_BRIDGE size_t mGBACoreReadAudio(struct mCore* core, int16_t* out, size_t outSamples) {
    if (!core || !out) return 0;
    struct mAudioBuffer* ab = core->getAudioBuffer(core);
    if (!ab) return 0;
    /* mAudioBufferDump 会按 channels 交错读取 */
    return mAudioBufferDump(ab, out, outSamples, 0);
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
    struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_WRONLY);
    if (!vf) return false;
    bool ret = mCoreSaveStateNamed(core, vf, 0);
    vf->close(vf);
    return ret;
}

/* 即时读档：从指定路径恢复整个模拟器状态 */
MGBA_BRIDGE bool mGBALoadStateFromFile(struct mCore* core, const char* path) {
    if (!core || !path) return false;
    struct VFile* vf = VFileOpen(path, O_RDONLY);
    if (!vf) return false;
    bool ret = mCoreLoadStateNamed(core, vf, 0);
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

/* 电池读档：从指定路径加载 SRAM */
MGBA_BRIDGE bool mGBALoadBatteryFromFile(struct mCore* core, const char* path) {
    if (!core || !path) return false;
    struct VFile* vf = VFileOpen(path, O_RDONLY);
    if (!vf) return false;
    bool ret = core->loadSave(core, vf);
    vf->close(vf);
    return ret;
}
