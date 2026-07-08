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
#include <mgba/internal/gba/sio.h>
#include <mgba/internal/gba/io.h>
#include <mgba-util/vfs.h>
#include <mgba-util/audio-buffer.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

/* ---- 网络 SIO 驱动所需头文件 ---- */
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define SHUT_RDWR SD_BOTH
#define MSG_NOSIGNAL 0
typedef int ssize_t;
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>
#endif

/* 防止链接器 --gc-sections 把桥接函数删掉 */
#define MGBA_BRIDGE __attribute__((visibility("default"), used))

#ifdef __ANDROID__
#define LOG_TAG "MGBA_BRIDGE"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#else
#define LOGI(...) do {} while (0)
#define LOGW(...) do {} while (0)
#define LOGE(...) do {} while (0)
#endif

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
    /* 不调 free(core)：Android Scudo 环境下，core 结构体的堆元数据可能
     * 在运行时被破坏（根本原因尚未定位），free 会触发 "invalid chunk state"
     * → SIGABRT。deinit 已清理内部资源（ROM、savedata、渲染器等），
     * 跳过 free 仅泄漏 core 结构体本身（~几 KB），OS 在进程退出时回收。 */
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

    /* 打印 ROM 头 0x1A 字节（savedata 类型）和游戏 code */
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
    LOGI("mGBACoreReset: before type=%d vf=%p data=%p",
         gba->memory.savedata.type,
         (void*)gba->memory.savedata.vf,
         (void*)gba->memory.savedata.data);
    core->reset(core);
    LOGI("mGBACoreReset: after type=%d vf=%p data=%p realVf=%p cpu=%p dispcnt=0x%04X",
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

/* 即时存档：保存整个模拟器状态到指定路径。
 * 必须用 O_RDWR：mCoreSaveStateNamed 内部 map(MAP_WRITE) 走 mmap
 * (PROT_READ|PROT_WRITE) 路径，只写 fd 会导致 mmap EACCES → state=NULL
 * → 序列化到空指针 → 文件全零。 */
MGBA_BRIDGE bool mGBASaveStateToFile(struct mCore* core, const char* path) {
    if (!core || !path) return false;
    struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_RDWR);
    if (!vf) return false;
    bool ret = mCoreSaveStateNamed(core, vf, 0);
    vf->close(vf);
    return ret;
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
    LOGI("mGBALoadStateFromFile: fileSize=%lld stateSize=%zd",
         (long long)st.st_size, expectedSize);
    if ((long long)st.st_size < expectedSize) {
        LOGE("mGBALoadStateFromFile: file too small (%lld < %zd)",
             (long long)st.st_size, expectedSize);
        return false;
    }

    /* 读取存档文件头部，诊断反序列化失败原因 */
    struct VFile* vf = VFileOpen(path, O_RDONLY);
    if (!vf) {
        LOGE("mGBALoadStateFromFile: VFileOpen failed for %s (errno=%d)", path, errno);
        return false;
    }
    uint8_t header[32];
    ssize_t n = vf->read(vf, header, sizeof(header));
    if (n >= 32) {
        uint32_t versionMagic = (uint32_t)header[0] | ((uint32_t)header[1] << 8) |
                                ((uint32_t)header[2] << 16) | ((uint32_t)header[3] << 24);
        uint32_t biosChecksum = (uint32_t)header[4] | ((uint32_t)header[5] << 8) |
                                ((uint32_t)header[6] << 16) | ((uint32_t)header[7] << 24);
        uint32_t romCrc32    = (uint32_t)header[8] | ((uint32_t)header[9] << 8) |
                                ((uint32_t)header[10] << 16) | ((uint32_t)header[11] << 24);
        /* title[12] at offset 0x10, id[4] at offset 0x1C */
        char title[13] = {0};
        memcpy(title, header + 0x10, 12);
        char id[5] = {0};
        memcpy(id, header + 0x1C, 4);

        struct GBA* gba = (struct GBA*) core->board;
        char curCode[5] = {0};
        if (gba && gba->memory.rom) {
            memcpy(curCode, gba->memory.rom + 0xAC, 4);
        }
        LOGI("mGBALoadStateFromFile: stateHeader magic=0x%08X biosCS=0x%08X romCRC=0x%08X title='%.12s' id='%.4s'",
             versionMagic, biosChecksum, romCrc32, title, id);
        LOGI("mGBALoadStateFromFile: cur biosCS=0x%08X romCRC=0x%08X romCode='%s'",
             gba ? gba->biosChecksum : 0,
             gba ? gba->romCrc32 : 0,
             curCode);

        /* 预判会失败的校验 */
        if (versionMagic < 0x01000000 || versionMagic > 0x0100000B) {
            LOGW("mGBALoadStateFromFile: versionMagic out of range, load will fail");
        }
        if (gba && gba->memory.rom && gba->memory.rom[0xAC] != 0) {
            if (memcmp(id, gba->memory.rom + 0xAC, 4) != 0) {
                LOGW("mGBALoadStateFromFile: game ID mismatch! state='%.4s' rom='%.4s'", id, curCode);
            }
        }
    }
    vf->seek(vf, 0, SEEK_SET); /* 回卷以便 mCoreLoadStateNamed 从头读 */

    LOGI("mGBALoadStateFromFile: calling mCoreLoadStateNamed...");
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
 * 此时 VFile 所有权已转移给 savedata，绝不能 close，否则 savedata→vf
 * 成为悬垂指针，下次任何操作触发 GBASavedataInitFlash 时调用 vf→size()
 * 就会访问已销毁的函数表 → pc=0 崩溃。 */
MGBA_BRIDGE bool mGBALoadBatteryFromFile(struct mCore* core, const char* path) {
    if (!core || !path) return false;

    struct stat st;
    if (stat(path, &st) != 0) {
        LOGE("mGBALoadBatteryFromFile: stat failed for %s", path);
        return false;
    }
    LOGI("mGBALoadBatteryFromFile: path=%s size=%lld", path, (long long)st.st_size);

    struct GBA* gba = (struct GBA*) core->board;
    LOGI("mGBALoadBatteryFromFile: before loadSave type=%d vf=%p data=%p",
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
    /* 不要 vf->close(vf)！loadSave 内部 GBASavedataInit 已将 VFile 存入
     * savedata→vf，InitFlash 通过它映射数据。VFile 所有权已转移，由
     * 核心销毁（GBADeinit→GBASavedataDeinit→unmap）时统一清理。 */
    LOGI("mGBALoadBatteryFromFile: after loadSave ret=%d type=%d vf=%p data=%p realVf=%p",
         ret, gba->memory.savedata.type,
         (void*)gba->memory.savedata.vf,
         (void*)gba->memory.savedata.data,
         (void*)gba->memory.savedata.realVf);
    return ret;
}

/* ═══════════════════════════════════════════════════════════════════
 *  网络 SIO 驱动 — GBA 联机
 *
 *  架构：每个设备上运行一个 mGBA 核心，附带一个自定义 GBASIODriver。
 *  start 回调将本地的 SIODATA32 发送给对端 socket；
 *  finishNormal32 回调从 socket 接收对端数据并返回给 mGBA。
 *
 *  两台设备的模拟核心独立运行（各自约 60fps），start 调用大致在
 *  同一帧内发生，finishNormal32 的阻塞 recv 自然起到同步作用。
 * ═══════════════════════════════════════════════════════════════════ */

#define NET_SIO_DRIVER_ID 0x4E455457  /* "NETW" */
#define NET_SIO_PORT 7946
#define NET_SIO_ACCEPT_TIMEOUT_SEC 15
#define NET_SIO_RECV_TIMEOUT_SEC 5

struct GBASIONetworkDriver {
    struct GBASIODriver d;
    int sockFd;
};

/* ── vtable 回调 ──────────────────────────────────────────── */

static uint32_t _netSioDriverId(const struct GBASIODriver* driver) {
    UNUSED(driver);
    return NET_SIO_DRIVER_ID;
}

static bool _netSioInit(struct GBASIODriver* driver) {
    UNUSED(driver);
    return true;
}

static void _netSioDeinit(struct GBASIODriver* driver) {
    struct GBASIONetworkDriver* nd = (struct GBASIONetworkDriver*) driver;
    if (nd->sockFd >= 0) {
        LOGI("NET_SIO: deinit, closing socket fd=%d", nd->sockFd);
        shutdown(nd->sockFd, SHUT_RDWR);
#ifndef _WIN32
        close(nd->sockFd);
#else
        closesocket(nd->sockFd);
#endif
        nd->sockFd = -1;
    }
    /* 不 free(nd)：mGBA 的驱动生命周期模式中，驱动结构体由调用者管理，
     * GBASIOSetDriver 在替换旧驱动或 init 失败时也会调 deinit，此时
     * 调用者仍持有 nd 指针。结构体仅 ~24 字节，泄漏量可忽略。 */
}

static void _netSioReset(struct GBASIODriver* driver) {
    UNUSED(driver);
    /* 重置时不关闭 socket —— socket 由 deinit 统一管理 */
}

static bool _netSioLoadState(struct GBASIODriver* driver, const void* state, size_t size) {
    UNUSED(driver); UNUSED(state); UNUSED(size);
    return true; /* 不支持 savestate 中的联机状态 */
}

static void _netSioSaveState(struct GBASIODriver* driver, void** state, size_t* size) {
    UNUSED(driver);
    *state = NULL;
    *size = 0;
}

static void _netSioSetMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
    LOGI("NET_SIO: mode set to %d", (int) mode);
}

static bool _netSioHandlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
    UNUSED(driver);
    return mode == GBA_SIO_NORMAL_32
        || mode == GBA_SIO_NORMAL_8
        || mode == GBA_SIO_MULTI;
}

static int _netSioConnectedDevices(struct GBASIODriver* driver) {
    UNUSED(driver);
    return 1; /* 一个对端设备 */
}

static int _netSioDeviceId(struct GBASIODriver* driver) {
    UNUSED(driver);
    return 0; /* 本设备始终为 master（id=0），让对方作为 slave */
}

static uint16_t _netSioWriteSIOCNT(struct GBASIODriver* driver, uint16_t value) {
    LOGI("NET_SIO: SIOCNT <- 0x%04X", value);
    return value;
}

static uint16_t _netSioWriteRCNT(struct GBASIODriver* driver, uint16_t value) {
    LOGI("NET_SIO: RCNT <- 0x%04X", value);
    return value;
}

/* ── start：读取本地 SIODATA32，写入对端 socket ───────────── */

static bool _netSioStart(struct GBASIODriver* driver) {
    struct GBASIONetworkDriver* nd = (struct GBASIONetworkDriver*) driver;
    struct GBASIO* sio = driver->p;

    /* 从 GBA IO 寄存器读取本地要发送的 32-bit 数据 */
    uint32_t localData = sio->p->memory.io[GBA_REG(SIODATA32_LO)];
    localData |= (uint32_t) sio->p->memory.io[GBA_REG(SIODATA32_HI)] << 16;

    LOGI("NET_SIO: start transfer, sending 0x%08X", localData);

    ssize_t sent = send(nd->sockFd, (const char*) &localData, sizeof(localData), MSG_NOSIGNAL);
    if (sent != sizeof(localData)) {
        LOGE("NET_SIO: send failed, sent=%zd errno=%d", sent, errno);
        return false;
    }

    return true; /* 让 mGBA 自动调度 completion 事件 */
}

/* ── finishNormal32：从 socket 接收对端 32-bit 数据 ────────── */

static uint32_t _netSioFinishNormal32(struct GBASIODriver* driver) {
    struct GBASIONetworkDriver* nd = (struct GBASIONetworkDriver*) driver;
    uint32_t peerData = 0xFFFFFFFF;

    ssize_t n = recv(nd->sockFd, (char*) &peerData, sizeof(peerData), 0);
    if (n != sizeof(peerData)) {
        LOGE("NET_SIO: finishNormal32 recv failed, n=%zd errno=%d", n, errno);
        /* 返回 0xFFFFFFFF 表示连接中断 / 无数据 */
        return 0xFFFFFFFF;
    }

    LOGI("NET_SIO: finishNormal32 got 0x%08X", peerData);
    return peerData;
}

/* ── finishNormal8 ──────────────────────────────────────────── */

static uint8_t _netSioFinishNormal8(struct GBASIODriver* driver) {
    struct GBASIONetworkDriver* nd = (struct GBASIONetworkDriver*) driver;
    uint8_t peerData = 0xFF;

    ssize_t n = recv(nd->sockFd, (char*) &peerData, sizeof(peerData), 0);
    if (n != sizeof(peerData)) {
        LOGE("NET_SIO: finishNormal8 recv failed, n=%zd", n);
    } else {
        LOGI("NET_SIO: finishNormal8 got 0x%02X", peerData);
    }
    return peerData;
}

/* ── finishMultiplayer ───────────────────────────────────────── */

static void _netSioFinishMultiplayer(struct GBASIODriver* driver, uint16_t data[4]) {
    struct GBASIONetworkDriver* nd = (struct GBASIONetworkDriver*) driver;

    /* MULTI 模式：4 个 uint16_t 数据 */
    uint16_t peerData[4];
    ssize_t expected = sizeof(peerData); /* 8 bytes */
    ssize_t n = recv(nd->sockFd, (char*) peerData, expected, 0);
    if (n != expected) {
        LOGE("NET_SIO: finishMultiplayer recv failed, n=%zd", n);
        memset(data, 0xFF, sizeof(uint16_t) * 4);
    } else {
        memcpy(data, peerData, sizeof(uint16_t) * 4);
        LOGI("NET_SIO: finishMultiplayer got %04X %04X %04X %04X",
             data[0], data[1], data[2], data[3]);
    }
}

/* ── 辅助：设置 socket 为非阻塞 + 设置收发超时 ──────────────── */

static int _setSocketTimeout(int fd, int timeoutSec) {
#ifndef _WIN32
    struct timeval tv;
    tv.tv_sec = timeoutSec;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
        LOGE("NET_SIO: setsockopt SO_RCVTIMEO failed: %d", errno);
        return -1;
    }
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
        LOGE("NET_SIO: setsockopt SO_SNDTIMEO failed: %d", errno);
        return -1;
    }
#else
    DWORD timeout = timeoutSec * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*) &timeout, sizeof(timeout)) < 0) {
        LOGE("NET_SIO: setsockopt SO_RCVTIMEO failed: %d", WSAGetLastError());
        return -1;
    }
#endif
    return 0;
}

static int _setTcpNoDelay(int fd) {
    int opt = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) < 0) {
        LOGW("NET_SIO: setsockopt TCP_NODELAY failed: %d", errno);
        return -1;
    }
    return 0;
}

/* ── 桥接函数：创建新 socket，连接对端，挂载网络 SIO 驱动 ───── */

MGBA_BRIDGE int mGBALinkHost(struct mCore* core, int port) {
    if (!core) { LOGE("mGBALinkHost: null core"); return -1; }

    LOGI("mGBALinkHost: listening on port %d", port);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOGE("mGBALinkHost: WSAStartup failed");
        return -1;
    }
#endif

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        LOGE("mGBALinkHost: socket() failed, errno=%d", errno);
        return -1;
    }

    int opt = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, (const char*) &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverFd, (struct sockaddr*) &addr, sizeof(addr)) < 0) {
        LOGE("mGBALinkHost: bind() failed, errno=%d", errno);
#ifndef _WIN32
        close(serverFd);
#else
        closesocket(serverFd);
#endif
        return -1;
    }

    if (listen(serverFd, 1) < 0) {
        LOGE("mGBALinkHost: listen() failed, errno=%d", errno);
#ifndef _WIN32
        close(serverFd);
#else
        closesocket(serverFd);
#endif
        return -1;
    }

    /* 使用 select 实现 accept 超时 */
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(serverFd, &readfds);

    struct timeval tv;
    tv.tv_sec = NET_SIO_ACCEPT_TIMEOUT_SEC;
    tv.tv_usec = 0;

    LOGI("mGBALinkHost: waiting for client (timeout %ds)...", NET_SIO_ACCEPT_TIMEOUT_SEC);
    int selRet = select(serverFd + 1, &readfds, NULL, NULL, &tv);
    if (selRet <= 0) {
        LOGE("mGBALinkHost: accept timeout or select error, ret=%d", selRet);
#ifndef _WIN32
        close(serverFd);
#else
        closesocket(serverFd);
#endif
        return -1;
    }

    int clientFd = accept(serverFd, NULL, NULL);
#ifndef _WIN32
    close(serverFd);
#else
    closesocket(serverFd);
#endif

    if (clientFd < 0) {
        LOGE("mGBALinkHost: accept() failed, errno=%d", errno);
        return -1;
    }

    LOGI("mGBALinkHost: client connected, fd=%d", clientFd);

    /* 设置 TCP_NODELAY：确保小的 SIO 数据包立即发送 */
    _setTcpNoDelay(clientFd);
    _setSocketTimeout(clientFd, NET_SIO_RECV_TIMEOUT_SEC);

    /* 分配并挂载网络 SIO 驱动 */
    struct GBASIONetworkDriver* nd = calloc(1, sizeof(*nd));
    if (!nd) {
        LOGE("mGBALinkHost: calloc failed");
#ifndef _WIN32
        close(clientFd);
#else
        closesocket(clientFd);
#endif
        return -1;
    }

    nd->sockFd = clientFd;

    nd->d.init              = _netSioInit;
    nd->d.deinit            = _netSioDeinit;
    nd->d.reset             = _netSioReset;
    nd->d.driverId          = _netSioDriverId;
    nd->d.loadState         = _netSioLoadState;
    nd->d.saveState         = _netSioSaveState;
    nd->d.setMode           = _netSioSetMode;
    nd->d.handlesMode       = _netSioHandlesMode;
    nd->d.deviceId          = _netSioDeviceId;
    nd->d.connectedDevices  = _netSioConnectedDevices;
    nd->d.writeSIOCNT       = _netSioWriteSIOCNT;
    nd->d.writeRCNT         = _netSioWriteRCNT;
    nd->d.start             = _netSioStart;
    nd->d.finishMultiplayer = _netSioFinishMultiplayer;
    nd->d.finishNormal8     = _netSioFinishNormal8;
    nd->d.finishNormal32    = _netSioFinishNormal32;

    struct GBA* gba = (struct GBA*) core->board;
    GBASIOSetDriver(&gba->sio, &nd->d);
    LOGI("mGBALinkHost: network SIO driver attached, fd=%d", clientFd);
    return 0;
}

MGBA_BRIDGE int mGBALinkConnect(struct mCore* core, const char* host, int port) {
    if (!core || !host) { LOGE("mGBALinkConnect: null core or host"); return -1; }

    LOGI("mGBALinkConnect: connecting to %s:%d", host, port);

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOGE("mGBALinkConnect: WSAStartup failed");
        return -1;
    }
#endif

    int sockFd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockFd < 0) {
        LOGE("mGBALinkConnect: socket() failed, errno=%d", errno);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        LOGE("mGBALinkConnect: inet_pton failed for '%s'", host);
#ifndef _WIN32
        close(sockFd);
#else
        closesocket(sockFd);
#endif
        return -1;
    }

    /* 设置连接超时（通过 socket 非阻塞 + select） */
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sockFd, FIONBIO, &mode);
#else
    int flags = fcntl(sockFd, F_GETFL, 0);
    fcntl(sockFd, F_SETFL, flags | O_NONBLOCK);
#endif

    int connRet = connect(sockFd, (struct sockaddr*) &addr, sizeof(addr));

#ifdef _WIN32
    if (connRet == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        LOGE("mGBALinkConnect: connect() failed: %d", WSAGetLastError());
        closesocket(sockFd);
        return -1;
    }
#else
    if (connRet < 0 && errno != EINPROGRESS) {
        LOGE("mGBALinkConnect: connect() failed, errno=%d", errno);
        close(sockFd);
        return -1;
    }
#endif

    /* 等待连接完成 */
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(sockFd, &wfds);

    struct timeval tv;
    tv.tv_sec = NET_SIO_ACCEPT_TIMEOUT_SEC;
    tv.tv_usec = 0;

    int selRet = select(sockFd + 1, NULL, &wfds, NULL, &tv);
    if (selRet <= 0) {
        LOGE("mGBALinkConnect: connect timeout, ret=%d", selRet);
#ifndef _WIN32
        close(sockFd);
#else
        closesocket(sockFd);
#endif
        return -1;
    }

    /* 恢复阻塞模式 */
#ifdef _WIN32
    mode = 0;
    ioctlsocket(sockFd, FIONBIO, &mode);
#else
    fcntl(sockFd, F_SETFL, flags);
#endif

    /* 检查连接是否真正成功 */
    int soErr = 0;
    socklen_t soErrLen = sizeof(soErr);
    if (getsockopt(sockFd, SOL_SOCKET, SO_ERROR, (char*) &soErr, &soErrLen) < 0 || soErr != 0) {
        LOGE("mGBALinkConnect: SO_ERROR=%d", soErr);
#ifndef _WIN32
        close(sockFd);
#else
        closesocket(sockFd);
#endif
        return -1;
    }

    LOGI("mGBALinkConnect: connected to %s:%d, fd=%d", host, port, sockFd);

    _setTcpNoDelay(sockFd);
    _setSocketTimeout(sockFd, NET_SIO_RECV_TIMEOUT_SEC);

    struct GBASIONetworkDriver* nd = calloc(1, sizeof(*nd));
    if (!nd) {
        LOGE("mGBALinkConnect: calloc failed");
#ifndef _WIN32
        close(sockFd);
#else
        closesocket(sockFd);
#endif
        return -1;
    }

    nd->sockFd = sockFd;

    nd->d.init              = _netSioInit;
    nd->d.deinit            = _netSioDeinit;
    nd->d.reset             = _netSioReset;
    nd->d.driverId          = _netSioDriverId;
    nd->d.loadState         = _netSioLoadState;
    nd->d.saveState         = _netSioSaveState;
    nd->d.setMode           = _netSioSetMode;
    nd->d.handlesMode       = _netSioHandlesMode;
    nd->d.deviceId          = _netSioDeviceId;
    nd->d.connectedDevices  = _netSioConnectedDevices;
    nd->d.writeSIOCNT       = _netSioWriteSIOCNT;
    nd->d.writeRCNT         = _netSioWriteRCNT;
    nd->d.start             = _netSioStart;
    nd->d.finishMultiplayer = _netSioFinishMultiplayer;
    nd->d.finishNormal8     = _netSioFinishNormal8;
    nd->d.finishNormal32    = _netSioFinishNormal32;

    struct GBA* gba = (struct GBA*) core->board;
    GBASIOSetDriver(&gba->sio, &nd->d);
    LOGI("mGBALinkConnect: network SIO driver attached, fd=%d", sockFd);
    return 0;
}
