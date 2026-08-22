#include <nds.h>
#include <nds/libversion.h>
#include <calico/nds/ntrcard.h>
#include <calico/nds/gbacart.h>
#include <calico/nds/scfg.h>
#include <calico/nds/env.h>
#include <fat.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    CONSOLE_DS,
    CONSOLE_DS_LITE,
    CONSOLE_IQUE_DS,
    CONSOLE_IQUE_DS_LITE,
    CONSOLE_DSI,
    CONSOLE_DSI_XL,
} ConsoleType;

#define FW_CONSOLE_TYPE 0x1D
#define FW_WIFI_VERSION 0x2F

#define FW_TYPE_DS           0xFF
#define FW_TYPE_DS_LITE      0x20
#define FW_TYPE_DSI          0x57
#define FW_TYPE_IQUE_DS      0x43
#define FW_TYPE_IQUE_DS_LITE 0x63

#define REFRESH_FRAMES (60 * 5)

static ConsoleType detectConsole(void) {
    static uint8_t type __attribute__((aligned(32))) = FW_TYPE_DS;
    static uint8_t wifi_version __attribute__((aligned(32)));

    readFirmware(FW_CONSOLE_TYPE, &type, 1);
    readFirmware(FW_WIFI_VERSION, &wifi_version, 1);

    switch (type) {
        case FW_TYPE_DS_LITE:
            return CONSOLE_DS_LITE;
        case FW_TYPE_IQUE_DS:
            return CONSOLE_IQUE_DS;
        case FW_TYPE_IQUE_DS_LITE:
            return CONSOLE_IQUE_DS_LITE;
        case FW_TYPE_DSI:
            // if (wifi_version >= 0x1C)
            //    return CONSOLE_3DS;
            if (wifi_version >= 0x18)
                return CONSOLE_DSI_XL;
            return CONSOLE_DSI;
        default:
            return CONSOLE_DS;
    }
}

static const char* getConsoleName(ConsoleType type) {
    switch (type) {
        case CONSOLE_DS:             return "DS";
        case CONSOLE_DS_LITE:        return "DS Lite";
        case CONSOLE_IQUE_DS:        return "iQue DS";
        case CONSOLE_IQUE_DS_LITE:   return "iQue DS Lite";
        case CONSOLE_DSI:            return "DSi";
        case CONSOLE_DSI_XL:         return "DSi XL";
        // case CONSOLE_3DS:            return "3DS";
        default:                     return "Unknown";
    }
}

static void getUserName(char* out) {
    u16 len = PersonalData->nameLen;
    if (len > 10)
        len = 10;

    for (u16 i = 0; i < len; i++) {
        s16 c = PersonalData->name[i];
        out[i] = (c >= 0x20 && c < 0x7F) ? (char)c : '?';
    }
    out[len] = '\0';
}

static void getFwInfo(char* ident, char* date) {
    static union {
        struct {
            u8 _pad[8];
            char id[4];
            u8 _r0[12];
            u8 bcdMin, bcdHour, bcdDay, bcdMonth, bcdYear;
        } f;
        u8 raw[32];
    } hdr __attribute__((aligned(32)));

    readFirmware(0x08, hdr.raw, sizeof(hdr.raw));

    memcpy(ident, hdr.f.id, 4);
    ident[4] = '\0';
    for (int i = 0; i < 4; i++) {
        if (ident[i] < 0x20 || ident[i] > 0x7E)
            ident[i] = '?';
    }

    sprintf(date, "20%02X-%02X-%02X", hdr.f.bcdYear, hdr.f.bcdMonth, hdr.f.bcdDay);
}

static void getMacAddress(u8 out[6]) {
    static u8 mac[6] __attribute__((aligned(32)));
    readFirmware(0x36, mac, sizeof(mac));
    memcpy(out, mac, sizeof(mac));
}

static unsigned getCpuSpeedMHz(void) {
    if (scfgIsPresent() && (REG_SCFG_CLK & SCFG_CLK_CPU_134MHz))
        return 134;
    return 67;
}

static unsigned getRamSizeMB(void) {
    if (!scfgIsPresent())
        return 4;

    switch (REG_SCFG_EXT & SCFG_EXT_RAM_MASK) {
        case SCFG_EXT_RAM_16MB: return 16;
        case SCFG_EXT_RAM_32MB: return 32;
        default:                return 4;
    }
}

static const char* getLanguageName(void) {
    static const char* langs[] = {
        "Japanese", "English", "French", "German",
        "Italian", "Spanish", "Chinese", "Unknown",
    };
    return langs[PersonalData->language & 7];
}

static const char* getThemeName(void) {
    static const char* themes[] = {
        "Gray", "Brown", "Red", "Pink",
        "Orange", "Yellow", "Yellow-Green", "Green",
        "Dark Green", "Teal", "Light Blue", "Blue",
        "Dark Blue", "Dark Purple", "Purple", "Red-Purple",
    };
    return themes[PersonalData->theme & 15];
}

static void getBattery(int* percent, const char** state) {
    unsigned st = pmGetBatteryState();
    *percent = PM_BATT_LEVEL(st) * 100 / 15;
    *state = (st & PM_BATT_CHARGING) ? "Charging" : "Discharging";
}

static void getSlot1(char* out) {
    if (scfgIsPresent()) {
        unsigned mc = REG_SCFG_MC & SCFG_MC_POWER_MASK;
        if (mc == SCFG_MC_POWER_OFF || mc == SCFG_MC_POWER_OFF_REQ) {
            scfgSetMcPower(true);
            for (int i = 0; i < 30; i++) {
                swiWaitForVBlank();
                if ((REG_SCFG_MC & SCFG_MC_POWER_MASK) == SCFG_MC_POWER_ON)
                    break;
            }
        }
    }

    sysSetCardOwner(true);

    if (!ntrcardOpen()) {
        strcpy(out, "Flashcard");
        return;
    }

    NtrChipId id;
    bool ok = false;
    if (ntrcardGetMode() == NtrCardMode_Main || ntrcardStartup(-1))
        ok = ntrcardGetChipId(&id);

    if (ok && id.raw != 0xFFFFFFFF && id.raw != 0) {
        u32 size = ntrcardCalcChipSize(id);
        sprintf(out, "Game Card (%lu MB)", (unsigned long)(size >> 20));
    } else {
        strcpy(out, "Empty");
    }

    ntrcardClose();
}

// TODO: Untested, dosent work with expantion cards on the melonDS.
static const char* getSlot2(void) {
    if (!gbacartOpen())
        return "Empty";

    bool present = *(vu16*)0x08000000 != *(vu16*)0x08020000;
    gbacartClose();
    return present ? "Cartridge" : "Empty";
}

static void fmtSize(char* out, u64 bytes) {
    if (bytes >= (1ULL << 30))
        sprintf(out, "%.1fG", (double)bytes / (1ULL << 30));
    else if (bytes >= (1 << 20))
        sprintf(out, "%.0fM", (double)bytes / (1 << 20));
    else
        sprintf(out, "%luK", (unsigned long)(bytes >> 10));
}

static void getStorage(char* out, bool* fatReady) {
    static const struct { const char* dev; const char* label; } devs[] = {
        { "sd:/",   "SD"   },
        { "nand:/", "NAND" },
        { "fat:/",  "FAT"  },
    };

    out[0] = '\0';
    if (!*fatReady)
        *fatReady = fatInitDefault();
    if (!*fatReady) {
        strcpy(out, "N/A");
        return;
    }

    for (unsigned i = 0; i < sizeof(devs) / sizeof(devs[0]); i++) {
        struct statvfs st;
        if (statvfs(devs[i].dev, &st) != 0)
            continue;

        u64 total = (u64)st.f_blocks * st.f_frsize;
        u64 used = (u64)(st.f_blocks - st.f_bfree) * st.f_frsize;
        char szTotal[16], szUsed[16];
        fmtSize(szTotal, total);
        fmtSize(szUsed, used);

        if (out[0] != '\0')
            strcat(out, "  ");
        sprintf(out + strlen(out), "%s %s/%s", devs[i].label, szUsed, szTotal);
    }

    if (out[0] == '\0')
        strcpy(out, "N/A");
}

#define PI_F 3.14159265f

typedef struct {
    u8 shade;
    s32 v[4][3];
} GearQuad;

static GearQuad gearQ1[17 * 10];
static GearQuad gearQ2[17 * 16];

static void qpush(GearQuad* arr, int* n, float sh,
    float ax, float ay, float az,
    float bx, float by, float bz,
    float cx, float cy, float cz,
    float dx, float dy, float dz) {
    GearQuad* g = &arr[(*n)++];
    const float p[4][3] = {
        { ax, ay, az }, { bx, by, bz }, { cx, cy, cz }, { dx, dy, dz },
    };
    g->shade = (u8)(sh * 255.0f);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 3; j++)
            g->v[i][j] = floattov16(p[i][j]);
}

static int genGear(GearQuad* out, float innerR, float outerR, float width,
    int teeth, float toothDepth) {
    int n = 0;
    float r0 = innerR;
    float r1 = outerR - toothDepth * 0.5f;
    float r2 = outerR + toothDepth * 0.5f;
    float hw = width * 0.5f;
    float seg = 2.0f * PI_F / teeth;

    for (int i = 0; i < teeth; i++) {
        float a = i * seg;
        for (int k = 0; k < 4; k++) {
            float a0 = a + k * seg / 4;
            float a1 = a + (k + 1) * seg / 4;
            float c0 = cosf(a0), s0 = sinf(a0);
            float c1 = cosf(a1), s1 = sinf(a1);
            qpush(out, &n, 1.00f, r0*c0, r0*s0,  hw, r0*c1, r0*s1,  hw, r1*c1, r1*s1,  hw, r1*c0, r1*s0,  hw);
            qpush(out, &n, 0.45f, r0*c0, r0*s0, -hw, r1*c0, r1*s0, -hw, r1*c1, r1*s1, -hw, r0*c1, r0*s1, -hw);
            qpush(out, &n, 0.60f, r0*c0, r0*s0, -hw, r0*c0, r0*s0,  hw, r0*c1, r0*s1,  hw, r0*c1, r0*s1, -hw);
        }

        float cA = cosf(a),          sA = sinf(a);
        float cB = cosf(a + seg/4),  sB = sinf(a + seg/4);
        float cC = cosf(a + seg/2),  sC = sinf(a + seg/2);
        float cD = cosf(a + 3*seg/4), sD = sinf(a + 3*seg/4);

        qpush(out, &n, 0.80f, r1*cA, r1*sA,  hw, r2*cB, r2*sB,  hw, r2*cC, r2*sC,  hw, r1*cD, r1*sD,  hw);
        qpush(out, &n, 0.40f, r1*cA, r1*sA, -hw, r1*cD, r1*sD, -hw, r2*cC, r2*sC, -hw, r2*cB, r2*sB, -hw);
        qpush(out, &n, 0.70f, r1*cA, r1*sA,  hw, r1*cA, r1*sA, -hw, r2*cB, r2*sB, -hw, r2*cB, r2*sB,  hw);
        qpush(out, &n, 0.95f, r2*cB, r2*sB,  hw, r2*cB, r2*sB, -hw, r2*cC, r2*sC, -hw, r2*cC, r2*sC,  hw);
        qpush(out, &n, 0.70f, r2*cC, r2*sC,  hw, r2*cC, r2*sC, -hw, r1*cD, r1*sD, -hw, r1*cD, r1*sD,  hw);
    }

    return n;
}

static void drawGear(const GearQuad* q, int n, u8 r, u8 g, u8 b) {
    glBegin(GL_QUADS);
    for (int i = 0; i < n; i++) {
        glColor3b(
            (u8)((r * q[i].shade) >> 8),
            (u8)((g * q[i].shade) >> 8),
            (u8)((b * q[i].shade) >> 8));
        for (int j = 0; j < 4; j++)
            glVertex3v16(q[i].v[j][0], q[i].v[j][1], q[i].v[j][2]);
    }
    glEnd();
}

static bool fsExists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void getCfw(char* out, bool fatReady, ConsoleType hw) {
    static const struct { const char* root; } roots[] = {
        { "sd:/" },
        { "fat:/" },
    };

    bool twilight = false;
    bool hiya = false;

    if (fatReady && hw >= CONSOLE_DSI) {
        for (unsigned i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
            char path[64];
            snprintf(path, sizeof(path), "%s_nds/TWiLightMenu", roots[i].root);
            twilight = twilight || fsExists(path);
            snprintf(path, sizeof(path), "%shiya.dsi", roots[i].root);
            hiya = hiya || fsExists(path);
        }
    }

    bool dldiActive = g_envExtraInfo->dldi_io_type != 0;
    bool unlaunch = false;
    if (hw >= CONSOLE_DSI && !scfgIsPresent() && !dldiActive)
        unlaunch = true;

    out[0] = '\0';
    if (unlaunch)
        strcat(out, "Unlaunch");
    if (twilight) {
        if (out[0] != '\0')
            strcat(out, ", ");
        strcat(out, "TWiLight Menu++");
    }
    if (hiya) {
        if (out[0] != '\0')
            strcat(out, ", ");
        strcat(out, "hiyaCFW");
    }
    if (out[0] == '\0')
        strcpy(out, "None");
}

static void refreshInfo(bool* fatReady) {
    char name[11], ident[5], date[12], slot1[32], storage[96], cfw[64];
    u8 mac[6];
    int battPct;
    const char* battState;

    getUserName(name);
    getFwInfo(ident, date);
    getMacAddress(mac);
    getSlot1(slot1);
    getStorage(storage, fatReady);
    getBattery(&battPct, &battState);

    ConsoleType hw = detectConsole();
    getCfw(cfw, *fatReady, hw);

    consoleClear();
    iprintf("\x1b[36mndsfetch\x1b[39m\n\n");
    iprintf("\x1b[32mConsole:\x1b[39m  %s\n", getConsoleName(hw));
    iprintf("\x1b[32mFW Date:\x1b[39m  %s\n", date);
    iprintf("\x1b[32mUser:\x1b[39m     %s\n", name);
    iprintf("\x1b[32mCPU 1:\x1b[39m    ARM9 @ %u MHz\n", getCpuSpeedMHz());
    iprintf("\x1b[32mCPU 2:\x1b[39m    ARM7 @ 33 MHz\n");
    iprintf("\x1b[32mMemory:\x1b[39m   %u MB RAM\n", getRamSizeMB());
    iprintf("\x1b[32mDisplays:\x1b[39m 256x192 (x2)\n");
    iprintf("\x1b[32mLang:\x1b[39m     %s\n", getLanguageName());
    iprintf("\x1b[32mTheme:\x1b[39m    %s\n", getThemeName());
    iprintf("\x1b[32mBattery:\x1b[39m  %d%% [%s]\n", battPct, battState);
    iprintf("\x1b[32mSlot-1:\x1b[39m   %s\n", slot1);
    unsigned char hasSlot2 = hw == CONSOLE_DS ||
            hw == CONSOLE_DS_LITE ||
            hw == CONSOLE_IQUE_DS ||
            hw == CONSOLE_IQUE_DS_LITE;
    if (hasSlot2)
        iprintf("\x1b[32mSlot-2:\x1b[39m   %s\n", getSlot2());
    iprintf("\x1b[32mStorage:\x1b[39m  %s\n", storage);
    iprintf("\x1b[32mCFW:\x1b[39m      %s\n", cfw);
    // iprintf("\x1b[32mMAC:\x1b[39m      %02X:%02X:%02X:%02X:%02X:%02X\n",
    //    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int main(void) {
    videoSetModeSub(MODE_0_2D);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleInit(
        NULL,
        0,
        BgType_Text4bpp,
        BgSize_T_256x256,
        31,
        0,
        false,
        true
    );

    powerOn(POWER_3D_CORE | POWER_MATRIX);
    videoSetMode(MODE_0_3D);
    lcdMainOnBottom();

    glInit();
    glClearColor(0, 0, 0, 31);
    glClearPolyID(63);
    glClearDepth(0x7FFF);
    glViewport(0, 0, 255, 191);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    gluPerspective(35, 256.0 / 192.0, 1.0, 40);

    bool fatReady = false;
    refreshInfo(&fatReady);

    int n1 = genGear(gearQ1, 0.35f, 1.0f, 0.5f, 10, 0.25f);
    int n2 = genGear(gearQ2, 0.5f, 1.6f, 0.5f, 16, 0.25f);

    const int tiltX = degreesToAngle(24);
    const int tiltY = degreesToAngle(24);
    int ang1 = 0;
    int frames = 0;

    while (1) {
        swiWaitForVBlank();
        scanKeys();

        if ((keysDown() & KEY_SELECT) || pmShouldReset())
            break;

        if (keysDown() & KEY_TOUCH)
            lcdSwap();

        if (++frames >= REFRESH_FRAMES) {
            frames = 0;
            refreshInfo(&fatReady);
        }

        ang1 = (ang1 + 64) & (DEGREES_IN_CIRCLE - 1);

        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        glTranslatef32(0, 0, floattof32(-7));
        glRotatef32i(tiltX, floattof32(1), 0, 0);
        glRotatef32i(tiltY, 0, floattof32(1), 0);

        glPolyFmt(POLY_ALPHA(31) | POLY_CULL_NONE);

        glPushMatrix();
        glTranslatef32(floattof32(-1.24f), 0, floattof32(0.005f));
        glRotatef32i(ang1, 0, 0, floattof32(1));
        drawGear(gearQ1, n1, 255, 64, 64);
        glPopMatrix(1);

        glPushMatrix();
        glTranslatef32(floattof32(1.24f), 0, floattof32(-0.005f));
        glRotatef32i((-ang1 * 10 / 16 + 1536) & (DEGREES_IN_CIRCLE - 1), 0, 0, floattof32(1));
        drawGear(gearQ2, n2, 64, 128, 255);
        glPopMatrix(1);

        glFlush(GL_WBUFFERING);
    }

    return 0;
}
