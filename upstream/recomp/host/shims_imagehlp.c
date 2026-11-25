// Minimal imagehlp.dll shims (MapAndLoad, UnMapAndLoad, ImageRvaToVa, ImageDirectoryEntryToData)
// used by NVSE's PluginChecker to inspect plugin DLL export directories without running DllMain.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "shims.h"

static uint16_t rd16(uint32_t a) { uint16_t v; memcpy(&v, MEM + a, 2); return v; }

static uint32_t rva_to_va_raw(uint32_t nt, uint32_t base, uint32_t rva) {
    if (!nt || !base || !rva) return 0;
    uint16_t nsec = rd16(nt + 6);
    uint16_t osz = rd16(nt + 20);
    uint32_t sec = nt + 24 + osz;
    for (uint16_t i = 0; i < nsec; i++, sec += 40) {
        uint32_t vs = G32(sec + 8);
        uint32_t va = G32(sec + 12);
        uint32_t rs = G32(sec + 16);
        uint32_t rp = G32(sec + 20);
        uint32_t span = vs > rs ? vs : rs;
        if (rva >= va && rva < va + span) {
            return base + rp + (rva - va);
        }
    }
    return 0;
}

SHIM(MapAndLoad) {
    const char *img_name = GSTR(ARG(c, 0));
    const char *dll_path = GSTR(ARG(c, 1));
    uint32_t li = ARG(c, 2);
    uint32_t dot_dll = ARG(c, 3);
    if (!img_name || !li) { SETERR(87); RET(c, 0, 5); return; }

    char combined[1024], path[1024];
    if (dll_path && dll_path[0] && !strchr(img_name, '\\') && !strchr(img_name, '/')) {
        size_t dl = strlen(dll_path);
        const char *sep = (dll_path[dl - 1] == '\\' || dll_path[dl - 1] == '/') ? "" : "\\";
        snprintf(combined, sizeof combined, "%s%s%s%s", dll_path, sep, img_name,
                 (dot_dll && !strchr(img_name, '.')) ? ".dll" : "");
    } else {
        snprintf(combined, sizeof combined, "%s%s", img_name,
                 (dot_dll && !strchr(img_name, '.')) ? ".dll" : "");
    }
    host_path(combined, path, sizeof path);

    FILE *f = fopen(path, "rb");
    if (!f) { SETERR(2); RET(c, 0, 5); return; }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fsz < 0x100) { fclose(f); SETERR(11); RET(c, 0, 5); return; }

    uint32_t buf = gpages((uint32_t)fsz);
    if (fread(MEM + buf, 1, (size_t)fsz, f) != (size_t)fsz) {
        fclose(f); SETERR(29); RET(c, 0, 5); return;
    }
    fclose(f);

    if (MEM[buf] != 'M' || MEM[buf + 1] != 'Z') { SETERR(11); RET(c, 0, 5); return; }
    uint32_t pe = G32(buf + 0x3C);
    if (pe + 24 + 96 + 16 * 8 > (uint32_t)fsz || memcmp(MEM + buf + pe, "PE\0\0", 4) != 0) {
        SETERR(11); RET(c, 0, 5); return;
    }

    uint32_t nt = buf + pe;
    uint16_t nsec = rd16(nt + 6);
    uint16_t osz = rd16(nt + 20);
    uint16_t chars = rd16(nt + 22);
    uint32_t secs = nt + 24 + osz;
    uint32_t img_sz = G32(nt + 24 + 56);
    uint32_t mod_name = gmalloc((uint32_t)strlen(combined) + 1);
    strcpy((char *)MEM + mod_name, combined);

    // x86 LOADED_IMAGE (48 bytes)
    memset(MEM + li, 0, 48);
    P32(li + 0x00, mod_name);
    P32(li + 0x04, 0);
    P32(li + 0x08, buf);
    P32(li + 0x0C, nt);
    P32(li + 0x10, secs);
    P32(li + 0x14, nsec);
    P32(li + 0x18, secs);
    P32(li + 0x1C, chars);
    MEM[li + 0x20] = 0; // fSystemImage
    MEM[li + 0x21] = 0; // fDOSImage
    MEM[li + 0x22] = 1; // fReadOnly
    MEM[li + 0x23] = 0; // Version
    P32(li + 0x24, li + 0x24); // Links.Flink
    P32(li + 0x28, li + 0x24); // Links.Blink
    P32(li + 0x2C, img_sz);
    RET(c, 1, 5);
}

SHIM(UnMapAndLoad) {
    uint32_t li = ARG(c, 0);
    if (li) {
        uint32_t nm = G32(li + 0x00);
        if (nm) gfree(nm);
        memset(MEM + li, 0, 48);
    }
    RET(c, 1, 1);
}

SHIM(ImageRvaToVa) {
    uint32_t nt = ARG(c, 0), base = ARG(c, 1), rva = ARG(c, 2);
    RET(c, rva_to_va_raw(nt, base, rva), 4);
}

SHIM(ImageDirectoryEntryToData) {
    uint32_t base = ARG(c, 0), mapped_as_img = ARG(c, 1), dir_idx = ARG(c, 2), size_out = ARG(c, 3);
    if (size_out) P32(size_out, 0);
    if (!base || dir_idx >= 16 || MEM[base] != 'M' || MEM[base + 1] != 'Z') { RET(c, 0, 4); return; }
    uint32_t nt = base + G32(base + 0x3C);
    if (memcmp(MEM + nt, "PE\0\0", 4) != 0) { RET(c, 0, 4); return; }
    uint32_t rva = G32(nt + 24 + 96 + 8u * dir_idx);
    uint32_t sz = G32(nt + 24 + 96 + 8u * dir_idx + 4);
    if (size_out) P32(size_out, sz);
    if (!rva) { RET(c, 0, 4); return; }
    RET(c, mapped_as_img ? (base + rva) : rva_to_va_raw(nt, base, rva), 4);
}

SHIM_TABLE(imagehlp_shims,
    E(MapAndLoad) E(UnMapAndLoad) E(ImageRvaToVa) E(ImageDirectoryEntryToData))
