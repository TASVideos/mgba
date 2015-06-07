#include <stdlib.h>
#include <stddef.h>
#include "util/common.h"
#include "gba/gba.h"
#include "gba/renderers/video-software.h"
#include "gba/serialize.h"
#include "gba/supervisor/overrides.h"
#include "gba/video.h"
#include "util/vfs.h"

#define EXP __declspec(dllexport)

/**
 * container_of - cast a member of a structure out to the containing structure
 * @ptr:        the pointer to the member.
 * @type:       the type of the container struct this is embedded in.
 * @member:     the name of the member within the struct.
 *
 */
#define container_of(ptr, type, member) ({                      \
        const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
        (type *)( (char *)__mptr - offsetof(type,member) );})

void ARMDebuggerEnter(struct ARMDebugger* u1, enum DebuggerEntryReason u2, struct DebuggerEntryInfo* u3) { }
struct VFile* VFileOpen(const char* path, int flags) { return NULL; }

typedef struct
{
    struct GBA gba;
    struct ARMCore cpu;
    struct GBAVideoSoftwareRenderer renderer;
    struct GBAAVStream stream;
    color_t vbuff[VIDEO_HORIZONTAL_PIXELS * VIDEO_VERTICAL_PIXELS];
    void* rom;
    struct VFile* romvf;
    char bios[16384];
    struct VFile* biosvf;
    char savedata[SIZE_CART_FLASH1M];
    struct VFile* sramvf;
    struct GBARotationSource rotsource;
    struct GBARTCSource rtcsource;
    struct GBALuminanceSource lumasource;
    int16_t tiltx;
    int16_t tilty;
    int16_t tiltz;
    int64_t time;
    uint8_t light;
} bizctx;

static int32_t GetX(struct GBARotationSource* rotationSource)
{
	const struct GBARotationSource *__mptr = (rotationSource);
	return ((bizctx *)((char *)__mptr - offsetof(bizctx, rotsource)))->tiltx << 16;
}
static int32_t GetY(struct GBARotationSource* rotationSource)
{
	const struct GBARotationSource *__mptr = (rotationSource);
	return ((bizctx *)((char *)__mptr - offsetof(bizctx, rotsource)))->tilty << 16;
}
static int32_t GetZ(struct GBARotationSource* rotationSource)
{
	const struct GBARotationSource *__mptr = (rotationSource);
	return ((bizctx *)((char *)__mptr - offsetof(bizctx, rotsource)))->tiltz << 16;
}
static uint8_t GetLight(struct GBALuminanceSource* luminanceSource)
{
	const struct GBARotationSource *__mptr = (luminanceSource);
	return ((bizctx *)((char *)__mptr - offsetof(bizctx, lumasource)))->light;
}
static time_t GetTime(struct GBARTCSource* rtcSource)
{
	const struct GBARotationSource *__mptr = (rtcSource);
	return ((bizctx *)((char *)__mptr - offsetof(bizctx, rtcsource)))->time;
}
static void Dummy(const void* unused) { }

static void logdebug(struct GBAThread* thread, enum GBALogLevel level, const char* format, va_list args)
{

}

EXP void BizDestroy(bizctx* ctx)
{
    ctx->romvf->close(ctx->romvf);
    ctx->romvf = NULL;
    free(ctx->rom);
    ctx->rom = NULL;
    if (ctx->biosvf != NULL)
        ctx->biosvf->close(ctx->biosvf);
    ctx->sramvf->close(ctx->sramvf);

    // TODO: this seems short.  is there anything else that needs to happen here?
    GBADestroy(&ctx->gba);
    free(ctx);
}

EXP bizctx* BizCreate(const void* bios)
{
    bizctx* ctx = calloc(1, sizeof(*ctx));
    memset(ctx->savedata, 0xff, sizeof(ctx->savedata));
    if (ctx)
    {
        GBACreate(&ctx->gba);
        ARMSetComponents(&ctx->cpu, &ctx->gba.d, 0, NULL);
        ARMInit(&ctx->cpu);
        // TODO: configuration
        ctx->gba.logLevel = 0;
        ctx->gba.logHandler = logdebug;
        // ctx->gba.stream = &ctx->stream;
        ctx->gba.idleOptimization = IDLE_LOOP_IGNORE;
        ctx->gba.realisticTiming = TRUE;
        ctx->gba.rtcSource = &ctx->rtcsource;
        ctx->gba.luminanceSource = &ctx->lumasource;
        ctx->gba.rotationSource = &ctx->rotsource;

        GBAVideoSoftwareRendererCreate(&ctx->renderer);
        ctx->renderer.outputBuffer = ctx->vbuff;
        ctx->renderer.outputBufferStride = VIDEO_HORIZONTAL_PIXELS;
        GBAVideoAssociateRenderer(&ctx->gba.video, &ctx->renderer.d);

        GBAAudioResizeBuffer(&ctx->gba.audio, 1024);
        blip_set_rates(ctx->gba.audio.left, GBA_ARM7TDMI_FREQUENCY, 44100);
        blip_set_rates(ctx->gba.audio.right, GBA_ARM7TDMI_FREQUENCY, 44100);

        if (bios)
        {
            memcpy(ctx->bios, bios, 16384);
            ctx->biosvf = VFileFromMemory(ctx->bios, 16384);
            if (!GBAIsBIOS(ctx->biosvf))
            {
                ctx->biosvf->close(ctx->biosvf);
                GBADestroy(&ctx->gba);
                free(ctx);
                return NULL;
            }
            GBALoadBIOS(&ctx->gba, ctx->biosvf);
        }

        ctx->rotsource.sample = Dummy;
        ctx->rotsource.readTiltX = GetX;
        ctx->rotsource.readTiltY = GetY;
        ctx->rotsource.readGyroZ = GetZ;
        ctx->lumasource.sample = Dummy;
        ctx->lumasource.readLuminance = GetLight;
        ctx->rtcsource.sample = Dummy;
        ctx->rtcsource.unixTime = GetTime;
    }
    return ctx;
}

EXP void BizReset(bizctx* ctx)
{
    ARMReset(&ctx->cpu);
}

EXP int BizLoad(bizctx* ctx, const void* data, int length)
{
    ctx->rom = malloc(length);
    if (!ctx->rom)
        return 0;

    memcpy(ctx->rom, data, length);
    ctx->romvf = VFileFromMemory(ctx->rom, length);

    if (!GBAIsROM(ctx->romvf))
    {
        ctx->romvf->close(ctx->romvf);
        ctx->romvf = NULL;
        free(ctx->rom);
        ctx->rom = NULL;
        return 0;
    }

    ctx->sramvf = VFileFromMemory(ctx->savedata, sizeof(ctx->savedata));

    GBALoadROM(&ctx->gba, ctx->romvf, ctx->sramvf, NULL);

    struct GBACartridgeOverride override;
	const struct GBACartridge* cart = (const struct GBACartridge*) ctx->gba.memory.rom;
	memcpy(override.id, &cart->id, sizeof(override.id));
	if (GBAOverrideFind(NULL, &override))
    {
		GBAOverrideApply(&ctx->gba, &override);
	}

    BizReset(ctx);
    return 1;
}

static void blit(void* dst_, const void* src_)
{
    // swap R&B, set top (alpha) byte
    const uint8_t* src = (const uint8_t*)src_;
    uint8_t* dst = (uint8_t*)dst_;

    uint8_t* dst_end = dst + VIDEO_HORIZONTAL_PIXELS * VIDEO_VERTICAL_PIXELS * BYTES_PER_PIXEL;

    while (dst < dst_end)
    {
        dst[2] = src[0] | src[0] >> 5;
        dst[1] = src[1] | src[1] >> 5;
        dst[0] = src[2] | src[2] >> 5;
        dst[3] = 0xff;
        dst += 4;
        src += 4;
        dst[2] = src[0] | src[0] >> 5;
        dst[1] = src[1] | src[1] >> 5;
        dst[0] = src[2] | src[2] >> 5;
        dst[3] = 0xff;
        dst += 4;
        src += 4;
        dst[2] = src[0] | src[0] >> 5;
        dst[1] = src[1] | src[1] >> 5;
        dst[0] = src[2] | src[2] >> 5;
        dst[3] = 0xff;
        dst += 4;
        src += 4;
        dst[2] = src[0] | src[0] >> 5;
        dst[1] = src[1] | src[1] >> 5;
        dst[0] = src[2] | src[2] >> 5;
        dst[3] = 0xff;
        dst += 4;
        src += 4;
    }
}

EXP void BizAdvance(bizctx* ctx, int keys, color_t* vbuff, int* nsamp, int16_t* sbuff,
    int64_t time, int16_t gyrox, int16_t gyroy, int16_t gyroz, uint8_t luma)
{
    ctx->gba.keySource = &keys;
    ctx->light = luma;
    ctx->time = time;
    ctx->tiltx = gyrox;
    ctx->tilty = gyroy;
    ctx->tiltz = gyroz;
    int frameCount = ctx->gba.video.frameCounter;
    while (frameCount == ctx->gba.video.frameCounter)
    {
        ARMRunLoop(&ctx->cpu);
    }
    blit(vbuff, ctx->vbuff);
    *nsamp = blip_samples_avail(ctx->gba.audio.left);
    if (*nsamp > 1024)
        *nsamp = 1024;
    blip_read_samples(ctx->gba.audio.left, sbuff, 1024, TRUE);
    blip_read_samples(ctx->gba.audio.right, sbuff + 1, 1024, TRUE);
}

struct MemoryAreas
{
    const void* bios;
    const void* wram;
    const void* iwram;
    const void* mmio;
    const void* palram;
    const void* vram;
    const void* oam;
    const void* rom;
};

EXP void BizGetMemoryAreas(bizctx* ctx, struct MemoryAreas* dst)
{
    dst->bios = ctx->gba.memory.bios;
    dst->wram = ctx->gba.memory.wram;
    dst->iwram = ctx->gba.memory.iwram;
    dst->mmio = ctx->gba.memory.io;
    dst->palram = ctx->gba.video.palette;
    dst->vram = ctx->gba.video.renderer->vram;
    dst->oam = ctx->gba.video.oam.raw;
    dst->rom = ctx->gba.memory.rom;
}

EXP int BizGetSaveRamSize(bizctx* ctx)
{
    switch (ctx->gba.memory.savedata.type)
    {
	case SAVEDATA_AUTODETECT:
	case SAVEDATA_FLASH1M:
		return SIZE_CART_FLASH1M;
	case SAVEDATA_FLASH512:
		return SIZE_CART_FLASH512;
	case SAVEDATA_EEPROM:
		return SIZE_CART_EEPROM;
	case SAVEDATA_SRAM:
		return SIZE_CART_SRAM;
	case SAVEDATA_FORCE_NONE:
    default:
		return 0;
	}
}

EXP void BizGetSaveRam(bizctx* ctx, void* data)
{
    memcpy(data, ctx->savedata, BizGetSaveRamSize(ctx));
}

EXP void BizPutSaveRam(bizctx* ctx, const void* data)
{
    memcpy(ctx->savedata, data, BizGetSaveRamSize(ctx));
}

EXP int BizGetStateSize()
{
    return sizeof(struct GBASerializedState);
}

EXP void BizGetState(bizctx* ctx, void* data)
{
    GBASerialize(&ctx->gba, data);
}

EXP void BizPutState(bizctx* ctx, const void* data)
{
    GBADeserialize(&ctx->gba, data);
}