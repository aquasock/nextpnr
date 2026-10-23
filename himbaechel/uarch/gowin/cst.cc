#include <boost/algorithm/string.hpp>
#include <algorithm>
#include <regex>
#include <utility>
#include <vector>

#include "log.h"
#include "nextpnr.h"
#include "util.h"

#define HIMBAECHEL_CONSTIDS "uarch/gowin/constids.inc"
#include "himbaechel_constids.h"
#include "himbaechel_helpers.h"

#include "cst.h"
#include "gowin.h"
#include "gowin_utils.h"

NEXTPNR_NAMESPACE_BEGIN

struct GowinCstReader
{
    Context *ctx;
    std::istream &in;
    GowinUtils gwu;

    GowinCstReader(Context *ctx, std::istream &in) : ctx(ctx), in(in) { gwu.init(ctx); };

    // A placement macro (`PLL_L[0]`) is resolved through the chipdb's own
    // `macro_bels` table, which apicula fills from the measured site bijection.
    // Nothing here knows where on a die a site sits.
    BelId belByMacro(const std::string &macro)
    {
        const Extra_chip_data_POD *extra =
                reinterpret_cast<const Extra_chip_data_POD *>(ctx->chip_info->extra_data.get());
        IdString want = ctx->id(macro);
        for (auto &entry : extra->macro_bels) {
            if (IdString(entry.name) == want) {
                return ctx->getBelByLocation(Loc(entry.x, entry.y, entry.z));
            }
        }
        return BelId();
    }

    const PadInfoPOD *pinLookup(const PadInfoPOD *list, const size_t len, const IdString idx)
    {
        for (size_t i = 0; i < len; i++) {
            const PadInfoPOD *pin = &list[i];
            if (IdString(pin->package_pin) == idx) {
                return pin;
            }
        }
        return nullptr;
    }

    Loc getLoc(std::smatch match, int maxX, int maxY)
    {
        int col = std::stoi(match[2]);
        int row = 1; // Top
        std::string side = match[1].str();
        if (side == "R") {
            row = col;
            col = maxX;
        } else if (side == "B") {
            row = maxY;
        } else if (side == "L") {
            row = col;
            col = 1;
        }
        int z = match[3].str()[0] - 'A' + BelZ::IOBA_Z;
        return Loc(col - 1, row - 1, z);
    }

    // INS_LOC "<cell>" {TOP,RIGHT,BOTTOM,LEFT}SIDE[<n>] pins one CLKDIV to one
    // lane of one HCLK block of that side.  The index is a block ordinal and a
    // lane packed together, `n = block * lanes_per_block + lane`
    // (SUG1018-1.7E Table 2-2): a side may carry more than one HCLK block --
    // the GW5A(S)(T)-138 has two on each of left, right and bottom -- and the
    // blocks of a side are numbered along it, so ordering the candidate bels by
    // their coordinate along the side is what turns the ordinal into a bel.
    //
    // The Arora V HCLK block has four lanes and one CLKDIV per lane, at
    // consecutive z; the earlier families have two sections at every other z,
    // one block per side, and reduce to the previous behaviour of this
    // function.
    BelId getConstrainedHCLKBel(std::smatch match, int maxX, int maxY)
    {
        int idx = std::stoi(match[3]);
        bool hclk_5A = gwu.has_5A_HCLK();
        int lanes_per_block = hclk_5A ? 4 : 2;
        int block_ordinal = idx / lanes_per_block;
        int lane = idx % lanes_per_block;
        int bel_z = BelZ::CLKDIV_0_Z + (hclk_5A ? lane : 2 * lane);

        std::string side = match[2].str();
        bool lr = (side == "LEFT") || (side == "RIGHT");
        int y_coord = (side == "BOTTOM") ? maxY - 1 : 0;
        int x_coord = (side == "RIGHT") ? maxX - 1 : 0;

        // (position along the side, bel), one entry per HCLK block of the side
        // that carries this lane.
        std::vector<std::pair<int, BelId>> blocks;
        for (auto &bel : ctx->getBelsInBucket(ctx->getBelBucketForCellType(id_CLKDIV))) {
            auto this_loc = ctx->getBelLocation(bel);
            if (this_loc.z != bel_z) {
                continue;
            }
            if (lr) { // left or right side, corners excluded
                if (this_loc.x == x_coord && this_loc.y != 0 && this_loc.y != maxY - 1) {
                    blocks.emplace_back(this_loc.y, bel);
                }
            } else { // top or bottom side
                if (this_loc.y == y_coord) {
                    blocks.emplace_back(this_loc.x, bel);
                }
            }
        }
        if (block_ordinal >= int(blocks.size())) {
            return BelId();
        }
        std::sort(blocks.begin(), blocks.end(),
                  [](const std::pair<int, BelId> &a, const std::pair<int, BelId> &b) { return a.first < b.first; });
        return blocks.at(block_ordinal).second;
    }

    // INS_LOC "<cell>" R<row>C<col>[<cls>][A|B] names a CLS half-slot in the tile
    // at (col-1, row-1).  gowin_arch_gen.py creates LUT<i> at z = i*2 and DFF<i>
    // at z = i*2+1, with i = cls*2 + (half == "B"), so the LUT of a half-slot and
    // its DFF are adjacent z's.  Returns BelId() when nothing there can hold the
    // cell.
    BelId getConstrainedCLSBel(CellInfo *cell, int col, int row, int cls, const std::string &half)
    {
        int x = col - 1;
        int y = row - 1;
        if (x < 0 || y < 0 || x >= ctx->getGridDimX() || y >= ctx->getGridDimY()) {
            return BelId();
        }
        int idx = cls * 2 + (half == "B" ? 1 : 0);
        int lut_z = BelZ::LUT0_Z + idx * 2;

        // A LUT-class cell can only take the LUT z, a DFF/latch-class cell only the
        // DFF z; anything else (ALU, MUX, ...) is tried on both and validated.
        std::vector<int> candidates;
        if (type_is_lut(cell->type)) {
            candidates.push_back(lut_z);
        } else if (type_is_dff(cell->type) || type_is_latch(cell->type)) {
            candidates.push_back(lut_z + 1);
        } else {
            candidates.push_back(lut_z);
            candidates.push_back(lut_z + 1);
        }
        for (int z : candidates) {
            BelId bel = ctx->getBelByLocation(Loc(x, y, z));
            if (bel != BelId() && ctx->isValidBelForCellType(cell->type, bel)) {
                return bel;
            }
        }
        return BelId();
    }

    bool run(void)
    {
        pool<std::pair<IdString, IdStringList>> constrained_cells;
        auto debug_cell = [this, &constrained_cells](IdString cellId, IdStringList belId) {
            if (ctx->debug) {
                constrained_cells.insert(std::make_pair(cellId, belId));
            }
        };
        pool<IdString> adc_ios; // bus#/X#Y#

        log_info("Reading constraints...\n");
        try {
            // If two locations are specified separated by commas (for differential I/O buffers),
            // only the first location is actually recognized and used.
            // And pin A will be Positive and pin B will be Negative in any case.
            std::regex iobre = std::regex("IO_LOC +\"([^\"]+)\" +([^ ,;]+)(, *[^ ;]+)? *;.*[\\s\\S]*");
            std::regex portre = std::regex("IO_PORT +\"([^\"]+)\" +([^;]+;).*[\\s\\S]*");
            std::regex port_attrre = std::regex("([^ =;]+=[^ =;]+) *([^;]*;)");
            std::regex iobelre = std::regex("IO([TRBL])([0-9]+)\\[?([A-Z])\\]?");
            std::regex inslocre =
                    std::regex("INS_LOC +\"([^\"]+)\" +R([0-9]+)C([0-9]+)\\[([0-9])\\]\\[([AB])\\] *;.*[\\s\\S]*");
            std::regex hclkre =
                    std::regex("INS_LOC +\"([^\"]+)\" +(TOP|RIGHT|BOTTOM|LEFT)SIDE\\[([0-7])\\] *;*[\\s\\S]*");
            std::regex clockre = std::regex("CLOCK_LOC +\"([^\"]+)\" +BUF([GS])(\\[([0-7])\\])?[^;]*;.*[\\s\\S]*");
            std::regex adcre = std::regex("USE_ADC_SRC +bus([0-9]) +IO([TRBL])([0-9]+) *;.*[\\s\\S]*");
            // Third INS_LOC spelling shipped by the vendor: a placement-macro name,
            // e.g. INS_LOC "u_Gowin_PLL_AE350/PLL_inst" PLL_R[0]; or INS_LOC "u/u_dll"
            // DDRDLLM_BL;.  Matched LAST so it can never steal inslocre or hclkre.
            std::regex inslocmacrore =
                    std::regex("INS_LOC +\"([^\"]+)\" +([A-Z][A-Z0-9_]*)(\\[([0-9])\\])? *;.*[\\s\\S]*");
            std::smatch match, match_attr, match_pinloc;
            std::string line, pinlines[2];
            std::vector<IdStringList> constrained_clkdivs;
            enum
            {
                ioloc,
                ioport,
                insloc,
                clock,
                hclk,
                adc,
                inslocmacro
            } cst_type;

            while (!in.eof()) {
                std::getline(in, line);
                boost::trim_right(line);
                if (line.empty()) {
                    continue;
                }

                cst_type = ioloc;
                if (!std::regex_match(line, match, iobre)) {
                    if (std::regex_match(line, match, portre)) {
                        cst_type = ioport;
                    } else {
                        if (std::regex_match(line, match, clockre)) {
                            cst_type = clock;
                        } else {
                            if (std::regex_match(line, match, inslocre)) {
                                cst_type = insloc;
                            } else {
                                if (std::regex_match(line, match, hclkre)) {
                                    cst_type = hclk;
                                } else {
                                    if (std::regex_match(line, match, adcre)) {
                                        cst_type = adc;
                                    } else if (std::regex_match(line, match, inslocmacrore)) {
                                        cst_type = inslocmacro;
                                    } else {
                                        if ((!line.empty()) && (line.rfind("//", 0) == std::string::npos)) {
                                            log_warning("Invalid constraint: %s\n", line.c_str());
                                        }
                                        continue;
                                    }
                                }
                            }
                        }
                    }
                }

                IdString net = ctx->id(match[1]);
                auto it = ctx->cells.find(net);
                if (cst_type != clock && cst_type != adc && it == ctx->cells.end()) {
                    log_info("Cell %s not found\n", net.c_str(ctx));
                    continue;
                }
                switch (cst_type) {
                case adc: { // USE_ADC_SRC bus# IOLOC
                    int col = std::stoi(match[3]);
                    int row = 1; // Top
                    std::string side = match[2].str();
                    if (side == "R") {
                        row = col;
                        col = ctx->getGridDimX();
                    } else if (side == "B") {
                        row = ctx->getGridDimY();
                    } else if (side == "L") {
                        row = col;
                        col = 1;
                    }
                    adc_ios.insert(ctx->idf("%d/X%dY%d", std::stoi(match[1]), col - 1, row - 1));
                } break;
                case clock: { // CLOCK name BUFG|S=#
                    std::string which_clock = match[2];
                    std::string lw = match[4];
                    int lw_idx = -1;
                    if (lw.length() > 0) {
                        lw_idx = atoi(lw.c_str());
                        log_info("lw_idx:%d\n", lw_idx);
                    }
                    if (which_clock.at(0) == 'S') {
                        auto ni = ctx->nets.find(net);
                        if (ni == ctx->nets.end()) {
                            log_info("Net %s not found\n", net.c_str(ctx));
                            continue;
                        }
                        // if (!allocate_longwire(ni->second.get(), lw_idx)) {
                        log_info("Can't use the long wires. The %s network will use normal routing.\n", net.c_str(ctx));
                        //}
                    } else {
                        auto ni = ctx->nets.find(net);
                        if (ni == ctx->nets.end()) {
                            log_info("Net %s not found\n", net.c_str(ctx));
                            continue;
                        }
                        if (ctx->debug) {
                            log_info("Mark net '%s' as CLOCK\n", net.c_str(ctx));
                        }
                        // XXX YES for now. May be put the number here
                        ni->second->attrs[id_CLOCK] = Property(std::string("YES"));
                    }
                } break;
                case ioloc: { // IO_LOC name pin
                    int nb_iter = 1;
                    IdString nets[2];

                    // Prepare pinlines and nets (default: one Pin, one LOC).
                    pinlines[0] = match[2];
                    nets[0] = ctx->id(match[1]);

                    // Differential case: one Pin (_p), two LOCs separated by a ','
                    if (match[3].length() > 0) {
                        nb_iter++;
                        // Uses second pin after removing ','
                        pinlines[1] = std::regex_replace(match[3].str(), std::regex("^,"), "");

                        // Replaces _p with _n in pinname.
                        std::string tmp = std::regex_replace(match[1].str(), std::regex("_p$"), "_n");

                        nets[1] = ctx->id(tmp);
                        it = ctx->cells.find(nets[1]);
                        if (cst_type != clock && it == ctx->cells.end()) {
                            log_info("Cell %s not found\n", nets[1].c_str(ctx));
                            continue;
                        }
                    }

                    for (int iter = 0; iter < nb_iter; iter++) {
                        IdString pinname = ctx->id(pinlines[iter]);
                        auto it = ctx->cells.find(nets[iter]);

                        const PadInfoPOD *belname =
                                pinLookup(ctx->package_info->pads.get(), ctx->package_info->pads.ssize(), pinname);
                        if (belname != nullptr) {
                            IdStringList bel = IdStringList::concat(IdString(belname->tile), IdString(belname->bel));
                            it->second->setAttr(IdString(ID_BEL), bel.str(ctx));
                            debug_cell(it->second->name, bel);
                        } else {
                            if (std::regex_match(pinlines[iter], match_pinloc, iobelre)) {
                                // may be it's IOx#[AB] style?
                                Loc loc = getLoc(match_pinloc, ctx->getGridDimX(), ctx->getGridDimY());
                                BelId bel = ctx->getBelByLocation(loc);
                                if (bel == BelId()) {
                                    log_error("Pin %s not found (TRBL style). \n", pinlines[iter].c_str());
                                }
                                it->second->setAttr(IdString(ID_BEL), std::string(ctx->nameOfBel(bel)));
                                debug_cell(it->second->name, ctx->getBelName(bel));
                            } else {
                                log_error("Pin %s not found (pin# style)\n", pinname.c_str(ctx));
                            }
                        }
                    }
                } break;
                case hclk: {
                    IdString cell_type = it->second->type;
                    if (cell_type != id_CLKDIV) {
                        log_error("Unsupported or invalid cell type %s for hclk\n", cell_type.c_str(ctx));
                    }
                    BelId hclk_bel = getConstrainedHCLKBel(match, ctx->getGridDimX(), ctx->getGridDimY());
                    if (hclk_bel != BelId()) {
                        auto hclk_bel_name = ctx->getBelName(hclk_bel);
                        if (std::find(constrained_clkdivs.begin(), constrained_clkdivs.end(), hclk_bel_name) !=
                            constrained_clkdivs.end()) {
                            log_error("Only one CLKDIV can be placed at %sSIDE[%s]\n", match[2].str().c_str(),
                                      match[3].str().c_str());
                        }
                        constrained_clkdivs.push_back(hclk_bel_name);
                        it->second->setAttr(id_BEL, ctx->getBelName(hclk_bel).str(ctx));
                        debug_cell(it->second->name, ctx->getBelName(hclk_bel));
                    } else {
                        log_error("No Bel of type CLKDIV found at constrained location %sSIDE[%s]\n",
                                  match[2].str().c_str(), match[3].str().c_str());
                    }
                } break;
                case insloc: { // INS_LOC name RrCc[cls][A|B]
                    int row = std::stoi(match[2]);
                    int col = std::stoi(match[3]);
                    int cls = std::stoi(match[4]);
                    std::string half = match[5].str();
                    BelId bel = getConstrainedCLSBel(it->second.get(), col, row, cls, half);
                    if (bel == BelId()) {
                        log_error("No Bel able to hold cell %s (type %s) at constrained location "
                                  "R%dC%d[%d][%s]\n",
                                  net.c_str(ctx), it->second->type.c_str(ctx), row, col, cls, half.c_str());
                    }
                    it->second->setAttr(id_BEL, ctx->getBelName(bel).str(ctx));
                    debug_cell(it->second->name, ctx->getBelName(bel));
                } break;
                case inslocmacro: { // INS_LOC name <MACRO>[idx]
                    // Placement macros are resolved through a named table of
                    // macro FAMILIES.  It is APPEND-ONLY: the DDRDLL* rows land
                    // with the DLL work; an unresolved macro is an error, never a
                    // silent IO_PORT fall-through.  The individual sites are not
                    // listed here -- `PLL_L[0]` is looked up in the chipdb's
                    // `macro_bels` table, which apicula fills from the measured
                    // site bijection, so this file carries no knowledge of any
                    // one die's geometry.
                    static const dict<std::string, IdString> macro_bel_type = {
                            {"PLL_L", id_PLL},
                            {"PLL_R", id_PLL},
                            {"PLL_B", id_PLL},
                            // One measured GW5AST-138C DDRDLL master.  The
                            // other three macros are deliberately absent:
                            // their port tiles were never isolated, so they
                            // must fall out as an unknown macro instead of
                            // resolving to a site this tree cannot justify.
                            {"DDRDLLM_BL", id_DDRDLL},
                    };
                    std::string macro = match[2].str();
                    auto mit = macro_bel_type.find(macro);
                    if (mit == macro_bel_type.end()) {
                        log_error("Unknown placement macro %s in INS_LOC for cell %s\n", macro.c_str(), net.c_str(ctx));
                    }
                    std::string handle = macro + "[" + match[4].str() + "]";
                    BelId bel = belByMacro(handle);
                    if (bel == BelId()) {
                        log_error("This device has no site %s for the placement macro in INS_LOC for cell %s\n",
                                  handle.c_str(), net.c_str(ctx));
                    }
                    if (ctx->getBelType(bel) != mit->second &&
                        !ctx->isValidBelForCellType(it->second->type, bel)) {
                        log_error("Bel %s (type %s) cannot hold cell %s (type %s)\n", ctx->nameOfBel(bel),
                                  ctx->getBelType(bel).c_str(ctx), net.c_str(ctx), it->second->type.c_str(ctx));
                    }
                    it->second->setAttr(id_BEL, ctx->getBelName(bel).str(ctx));
                    debug_cell(it->second->name, ctx->getBelName(bel));
                } break;
                default: { // IO_PORT attr=value
                    std::string attr_val = match[2];
                    while (std::regex_match(attr_val, match_attr, port_attrre)) {
                        std::string attr = "&";
                        attr += match_attr[1];
                        boost::algorithm::to_upper(attr);
                        it->second->setAttr(ctx->id(attr), 1);
                        attr_val = match_attr[2];
                    }
                }
                }
            }
            if (ctx->debug) {
                for (auto &cell : constrained_cells) {
                    log_info("Cell %s is constrained to %s\n", cell.first.c_str(ctx), cell.second.str(ctx).c_str());
                }
                if (!adc_ios.empty()) {
                    log_info("ADC iobufs:\n");
                    for (auto &bus_io : adc_ios) {
                        log_info("  bus %s\n", bus_io.c_str(ctx));
                    }
                }
            }
            if (!adc_ios.empty()) {
                for (auto &cell : ctx->cells) {
                    auto &ci = *cell.second;

                    if (is_adc(&ci)) {
                        int idx = 0;
                        for (auto &bus_io : adc_ios) {
                            ci.setAttr(ctx->idf("ADC_IO_%d", idx), bus_io.str(ctx));
                            ++idx;
                        }
                    }
                }
            }
            return true;
        } catch (log_execution_error_exception) {
            return false;
        }
    }
};

static void add_sip_constraints(Context *ctx, const Extra_package_data_POD *extra)
{
    for (auto cst : extra->cst) {
        auto it = ctx->cells.find(IdString(cst.net));
        if (it == ctx->cells.end()) {
            log_info("Cell %s not found\n", IdString(cst.net).c_str(ctx));
            continue;
        }
        Loc loc = Loc(cst.col, cst.row, cst.bel);
        BelId bel = ctx->getBelByLocation(loc);
        if (bel == BelId()) {
            log_error("Pin not found.\n");
        }
        it->second->setAttr(IdString(ID_BEL), std::string(ctx->nameOfBel(bel)));

        if (cst.iostd > 0) {
            std::string attr = "&IO_TYPE=";
            attr += IdString(cst.iostd).c_str(ctx);
            boost::algorithm::to_upper(attr);
            it->second->setAttr(ctx->id(attr), 1);
        }
    }
}

bool gowin_apply_constraints(Context *ctx, std::istream &in)
{
    // implicit constraints from SiP pins
    if (!ctx->package_info->extra_data.is_null()) {
        const Extra_package_data_POD *extra =
                reinterpret_cast<const Extra_package_data_POD *>(ctx->package_info->extra_data.get());
        add_sip_constraints(ctx, extra);
    }

    GowinCstReader reader(ctx, in);
    return reader.run();
}

NEXTPNR_NAMESPACE_END
