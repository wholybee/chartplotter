#pragma once
// src/portrayal_binary.hpp
//
// On-disk record layout of the prebaked symbol/portrayal binary (symbols.bin,
// magic "SYM\x06"). MUST match tools/gen_symbols.cpp exactly. Shared by the
// loaders that split the file into portrayal rules (PortrayalPackage) and
// render resources (RenderResourceAtlas).

#include <cstdint>

#pragma pack(push, 1)
struct BinHeader {
    char     magic[4];
    uint32_t symCount, lupCount, condCount, attrCount;
    uint32_t colorCount, lcCount, apCount, strBytes;
};
struct BinSymRecord {
    char    name[24];
    int16_t atlas_x, atlas_y, width, height, pivot_x, pivot_y;
};
struct BinLupRecord {
    char     objClass[8];
    uint8_t  geomType, dispCat, nConds, _pad;
    uint16_t condStart, _pad2;
    uint32_t instrOff;
    uint16_t instrLen, _pad3;
};
struct BinCondRecord { char attr[8]; char value[24]; };
struct BinAttrRecord { char acronym[8]; };
struct BinColorRecord { char token[8]; uint8_t r, g, b, a; };
struct BinLcDefRecord {
    char     name[24];
    uint8_t  r, g, b, a;
    uint16_t vecW, vecH, pivotX, pivotY, originX, originY;
    uint32_t hpglOff; uint16_t hpglLen, _pad;
};
struct BinApDefRecord {
    char     name[24];
    uint8_t  r, g, b, a;
    uint16_t vecW, vecH, pivotX, pivotY, originX, originY;
    uint16_t minDist, maxDist;
    uint8_t  fillType, spacing, hasBitmap, _pad;
    int16_t  bmpX, bmpY, bmpW, bmpH;
    uint32_t hpglOff; uint16_t hpglLen, _pad2;
};
#pragma pack(pop)

// These must match tools/gen_symbols.cpp exactly (binary format "SYM\x06").
static_assert(sizeof(BinHeader)      == 36, "BinHeader size");
static_assert(sizeof(BinSymRecord)   == 36, "BinSymRecord size");
static_assert(sizeof(BinLupRecord)   == 24, "BinLupRecord size");
static_assert(sizeof(BinCondRecord)  == 32, "BinCondRecord size");
static_assert(sizeof(BinAttrRecord)  == 8,  "BinAttrRecord size");
static_assert(sizeof(BinColorRecord) == 12, "BinColorRecord size");
static_assert(sizeof(BinLcDefRecord) == 48, "BinLcDefRecord size");
static_assert(sizeof(BinApDefRecord) == 64, "BinApDefRecord size");
