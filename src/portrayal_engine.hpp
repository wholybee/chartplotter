#pragma once
// src/portrayal_engine.hpp
//
// Stage 4: S-52 portrayal package + evaluator (renderer architecture plan:
// PortrayalPackage + PortrayalEngine).
//
// PortrayalPackage is the data half: the per-class lookup tables (LUPs) with
// their attribute conditions, the instruction string blob, the colour table,
// and the list of attribute acronyms any rule references. It is a swappable
// data package — a different portrayal (S-101, an updated S-52 edition) is a
// different package, with no change to the evaluator or the renderer.
//
// PortrayalEngine is the logic half: it scores a feature's attributes against
// the package's LUPs, executes the chosen S-52 instruction (including the
// conditional-symbology CS procedures), and emits a renderer-neutral SymHit
// (portrayal_ir.hpp). It resolves symbol / line-complex / area-pattern names
// through a RenderResourceAtlas but never draws anything itself.
//
// Thread safety: after the package is loaded its data is immutable. A
// PortrayalEngine is a lightweight value bound to a const package + atlas; many
// engines can evaluate concurrently on worker threads.

#include <QByteArray>
#include <QColor>
#include <QHash>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "portrayal_ir.hpp"
#include "portrayal_binary.hpp"

class RenderResourceAtlas;

class PortrayalPackage {
public:
    struct Lup {
        uint8_t  geom;
        uint8_t  dispCat;
        uint8_t  nConds;
        uint16_t condStart;
        uint32_t instrOff;
        uint16_t instrLen;
    };
    struct Cond { std::string attr; std::string value; };

    // Populate from the parsed binary sections. `strBase`/`strBytes` is the
    // shared string blob holding the instruction text.
    void load(const BinLupRecord* lupRecs, uint32_t lupCount,
              const BinCondRecord* condRecs, uint32_t condCount,
              const BinAttrRecord* attrRecs, uint32_t attrCount,
              const BinColorRecord* colorRecs, uint32_t colorCount,
              const char* strBase, std::size_t strBytes);

    // The S-57 attribute acronyms that any lookup condition, text command, or CS
    // procedure references. The chart loader reads exactly these per feature.
    const std::vector<std::string>& relevantAttrs() const { return attrs_; }

    // S-52 colour token (e.g. "CHBLK", "LITRD") -> colour, black if unknown.
    QColor colorFor(const QByteArray& token) const;

    // Contiguous [first, first+count) LUP range for an object-class+geom key,
    // or false if the class/geom has no lookups.
    bool lookupRange(const QByteArray& key, uint32_t& first, uint32_t& count) const;
    const Lup&  lup(uint32_t i)  const { return lups_[i]; }
    const Cond& cond(uint32_t i) const { return conds_[i]; }

    // Instruction bytes at [off, off+len), or empty if the range is invalid.
    QByteArray instruction(uint32_t off, uint16_t len) const;

    static QByteArray key(const QByteArray& objClass, SymGeom geom);

private:
    std::vector<Lup>          lups_;
    std::vector<Cond>         conds_;
    std::vector<std::string>  attrs_;
    std::string               strBlob_;
    QHash<QByteArray, QColor> colorTable_;
    QHash<QByteArray, std::pair<uint32_t, uint32_t>> lupIndex_;
};

class PortrayalEngine {
public:
    PortrayalEngine(const PortrayalPackage& pkg, const RenderResourceAtlas& res)
        : pkg_(pkg), res_(res) {}

    // Resolve a feature (object class + geometry + attributes) to a SymHit via
    // S-52 best-match selection plus instruction execution (including CS).
    SymHit evaluate(const QByteArray& objClass, SymGeom geom,
                    const PortrayalAttrs& attrs) const;

private:
    void execInstruction(const QByteArray& instr, const QByteArray& objClass,
                         SymGeom geom, const PortrayalAttrs& attrs, SymHit& hit,
                         int depth) const;
    void runCS(const QByteArray& proc, const QByteArray& objClass, SymGeom geom,
               const PortrayalAttrs& attrs, SymHit& hit, int depth) const;
    static const std::string* featVal(const PortrayalAttrs& a, const char* acronym);

    const PortrayalPackage&   pkg_;
    const RenderResourceAtlas& res_;
};
