#include <map>

#include "design_utils.h"
#include "log.h"
#include "nextpnr.h"

#define HIMBAECHEL_CONSTIDS "uarch/gowin/constids.inc"
#include "himbaechel_constids.h"
#include "himbaechel_helpers.h"

#include "gowin.h"
#include "gowin_utils.h"
#include "pack.h"

#include <cinttypes>

NEXTPNR_NAMESPACE_BEGIN

// ===================================
// Global set/reset
// ===================================
void GowinPacker::pack_gsr(void)
{
    log_info("Pack GSR...\n");

    bool user_gsr = false;
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;

        if (ci.type == id_GSR) {
            user_gsr = true;
            break;
        }
    }
    if (!user_gsr) {
        // make default GSR
        auto gsr_cell = std::make_unique<CellInfo>(ctx, id_GSR, id_GSR);
        gsr_cell->addInput(id_GSRI);
        gsr_cell->connectPort(id_GSRI, ctx->nets.at(ctx->id("$PACKER_VCC")).get());
        ctx->cells[gsr_cell->name] = std::move(gsr_cell);
    }
    if (ctx->verbose) {
        if (user_gsr) {
            log_info("Have user GSR\n");
        } else {
            log_info("No user GSR. Make one.\n");
        }
    }
}

// ===================================
// Pin function configuration via wires
// ===================================
void GowinPacker::pack_pincfg(void)
{
    if (!gwu.has_PINCFG()) {
        return;
    }
    log_info("Pack PINCFG...\n");

    auto pincfg_cell = std::make_unique<CellInfo>(ctx, id_PINCFG, id_PINCFG);

    const int pin_cnt = gwu.has_I2CCFG() ? 5 : 4;
    for (int i = 0; i < pin_cnt; ++i) {
        IdString port = ctx->idf("UNK%d_VCC", i);
        pincfg_cell->addInput(port);
        if (i && gwu.need_CFGPINS_INVERSION()) {
            pincfg_cell->connectPort(port, ctx->nets.at(ctx->id("$PACKER_GND")).get());
        } else {
            pincfg_cell->connectPort(port, ctx->nets.at(ctx->id("$PACKER_VCC")).get());
        }
    }

    const ArchArgs &args = ctx->args;

    pincfg_cell->addInput(id_SSPI);
    if (args.options.count("sspi_as_gpio")) {
        pincfg_cell->connectPort(id_SSPI, ctx->nets.at(ctx->id("$PACKER_VCC")).get());
        pincfg_cell->setParam(id_SSPI, 1);
    } else {
        pincfg_cell->connectPort(id_SSPI, ctx->nets.at(ctx->id("$PACKER_GND")).get());
    }

    if (gwu.has_I2CCFG()) {
        pincfg_cell->addInput(id_I2C);
        if (args.options.count("i2c_as_gpio")) {
            pincfg_cell->connectPort(id_I2C, ctx->nets.at(ctx->id("$PACKER_VCC")).get());
            pincfg_cell->setParam(id_I2C, 1);
        } else {
            pincfg_cell->connectPort(id_I2C, ctx->nets.at(ctx->id("$PACKER_GND")).get());
        }
    }
    ctx->cells[pincfg_cell->name] = std::move(pincfg_cell);
}

// ===================================
// Global power regulator
// ===================================
void GowinPacker::pack_bandgap(void)
{
    if (!gwu.has_BANDGAP()) {
        return;
    }
    log_info("Pack BANDGAP...\n");

    bool user_bandgap = false;
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;

        if (ci.type == id_BANDGAP) {
            user_bandgap = true;
            break;
        }
    }
    if (!user_bandgap) {
        // make default BANDGAP
        auto bandgap_cell = std::make_unique<CellInfo>(ctx, id_BANDGAP, id_BANDGAP);
        bandgap_cell->addInput(id_BGEN);
        bandgap_cell->connectPort(id_BGEN, ctx->nets.at(ctx->id("$PACKER_VCC")).get());
        ctx->cells[bandgap_cell->name] = std::move(bandgap_cell);
    }
    if (ctx->verbose) {
        if (user_bandgap) {
            log_info("Have user BANDGAP\n");
        } else {
            log_info("No user BANDGAP. Make one.\n");
        }
    }
}

// ===================================
// Replace INV with LUT
// ===================================
void GowinPacker::pack_inv(void)
{
    log_info("Pack INV...\n");

    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;

        if (ci.type == id_INV) {
            ci.type = id_LUT4;
            ci.renamePort(id_O, id_F);
            ci.renamePort(id_I, id_I3); // use D - it's simple for INIT
            ci.params[id_INIT] = Property(0x00ff);
        }
    }
}

// A die does not have to wire every port its PLL primitive declares: the
// GW5AST-138C's `.dat` gives no wire at all for the delay-trim inputs
// (`DT0` .. `DT3`), which the `PLL` port list still carries and which an RTL
// instantiation still has to tie off.  A port with no wire cannot be routed,
// so a *constant* tie-off on one is dropped here -- routing it could not have
// achieved anything else.  A port carrying a real signal is left connected and
// still fails in the router, which is the correct outcome for a design that
// asks this device for a function it does not have.
void GowinPacker::drop_unwired_constant_inputs(CellInfo *ci)
{
    BelId ref_bel;
    for (BelId bel : ctx->getBels()) {
        if (ctx->getBelType(bel) == ci->type) {
            ref_bel = bel;
            break;
        }
    }
    if (ref_bel == BelId()) {
        return;
    }
    std::vector<IdString> unwired;
    for (auto &port : ci->ports) {
        if (port.second.type != PORT_IN || port.second.net == nullptr) {
            continue;
        }
        if (ctx->getBelPinWire(ref_bel, port.first) != WireId()) {
            continue;
        }
        IdString net = port.second.net->name;
        if (net != ctx->id("$PACKER_GND") && net != ctx->id("$PACKER_VCC")) {
            continue;
        }
        unwired.push_back(port.first);
    }
    for (IdString port : unwired) {
        ci->disconnectPort(port);
    }
    if (!unwired.empty()) {
        log_info("    %s: dropped %zu constant tie-off(s) on ports this device does not wire\n",
                 ctx->nameOf(ci), unwired.size());
    }
}

// A `PLL` output's enable, `ENCLK<n>`, is a routed input on the GW5AST-138C
// (TinyVCMI tools/pll_enclk_probe.py): a signal or a constant reaches it on
// an ordinary pip and no PLL fuse moves.  Every open-flow PLL image built
// before the port was modelled left it undriven and its outputs ran, so the
// pin's idle state is "enabled".  Routing a constant that means the same
// thing would only change those images, so a tie-off is dropped when it is
// high, or low on an output nothing uses; a signal, and a low tie on a used
// output, which asks for that output to stop, are routed.
void GowinPacker::drop_idle_output_enables(CellInfo *ci)
{
    std::vector<IdString> idle;
    for (int i = 0; i < 7; ++i) {
        IdString port = ctx->idf("ENCLK%d", i);
        NetInfo *net = ci->getPort(port);
        if (net == nullptr) {
            continue;
        }
        NetInfo *out = ci->getPort(ctx->idf("CLKOUT%d", i));
        bool used = out != nullptr && !out->users.empty();
        if (net->name == ctx->id("$PACKER_VCC") || (net->name == ctx->id("$PACKER_GND") && !used)) {
            idle.push_back(port);
        }
    }
    for (IdString port : idle) {
        ci->disconnectPort(port);
    }
}

// ===================================
// PLL
// ===================================
void GowinPacker::pack_pll(void)
{
    log_info("Pack PLL...\n");

    pool<BelId> used_pll_bels;

    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;

        if (ci.type.in(id_rPLL, id_PLLVR, id_PLLA, id_PLL)) {
            gwu.remove_brackets(&ci);
            drop_unwired_constant_inputs(&ci);
            if (ci.type == id_PLL) {
                drop_idle_output_enables(&ci);
            }

            // If CLKIN is connected to a special pin, then it makes sense
            // to try to place the PLL so that it uses a direct connection
            // to this pin.
            if (ci.bel == BelId()) {
                NetInfo *ni = ci.getPort(id_CLKIN);
                if (ni && ni->driver.cell && ni->driver.cell->bel != BelId()) {
                    BelId pll_bel = gwu.get_pll_bel(ni->driver.cell->bel, id_CLKIN_T);
                    if (ctx->debug) {
                        log_info("PLL clkin driver:%s at %s, PLL bel:%s\n", ctx->nameOf(ni->driver.cell),
                                 ctx->getBelName(ni->driver.cell->bel).str(ctx).c_str(),
                                 pll_bel != BelId() ? ctx->getBelName(pll_bel).str(ctx).c_str() : "NULL");
                    }
                    if (pll_bel != BelId() && used_pll_bels.count(pll_bel) == 0) {
                        used_pll_bels.insert(pll_bel);
                        ctx->bindBel(pll_bel, &ci, PlaceStrength::STRENGTH_LOCKED);
                        ci.disconnectPort(id_CLKIN);
                        ci.setParam(id_INSEL, std::string("CLKIN0"));
                    }
                }
            }
        }
    }
}

// ===================================
// ADC
// ===================================
void GowinPacker::pack_adc(void)
{
    log_info("Pack ADC...\n");

    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;

        if (is_adc(&ci)) {
            gwu.remove_brackets(&ci);
        }
    }
}

// ===================================
// HCLK -- CLKDIV and CLKDIV2 for now
// ===================================
void GowinPacker::pack_hclk(void)
{
    log_info("Pack HCLK cells...\n");

    // In the GW5A series, the CLKDIV2 can simultaneously transmit a signal to
    // both IOLOGIC and CLKDIV.
    if (gwu.has_5A_HCLK()) {
        return;
    }
    for (auto &cell : ctx->cells) {
        auto ci = cell.second.get();
        if (ci->type != id_CLKDIV)
            continue;
        NetInfo *hclk_in = ci->getPort(id_HCLKIN);
        if (hclk_in) {
            CellInfo *this_driver = hclk_in->driver.cell;
            if (this_driver && this_driver->type == id_CLKDIV2) {
                NetInfo *out = this_driver->getPort(id_CLKOUT);
                if (out->users.entries() > 1) {
                    // We could do as the IDE does sometimes and replicate the CLKDIV2 cell
                    // as many times as we need. For now, we keep things simple
                    log_error("CLKDIV2 that drives CLKDIV should drive no other cells\n");
                }
                ci->cluster = ci->name;
                this_driver->cluster = ci->name;
                ci->constr_children.push_back(this_driver);
                this_driver->constr_x = 0;
                this_driver->constr_y = 0;
                this_driver->constr_z = BelZ::CLKDIV2_0_Z - BelZ::CLKDIV_0_Z;
                this_driver->constr_abs_z = false;
            }
        }
    }
}

// ===================================
// DLLDLY
// ===================================
void GowinPacker::pack_dlldly(void)
{
    log_info("Pack DLLDLYs...\n");

    for (auto &cell : ctx->cells) {
        auto ci = cell.second.get();
        if (ci->type != id_DLLDLY)
            continue;
        NetInfo *clkin_net = ci->getPort(id_CLKIN);
        NetInfo *clkout_net = ci->getPort(id_CLKOUT);
        if (clkin_net == nullptr || clkout_net == nullptr) {
            log_error("%s cell has unconnected CLKIN or CLKOUT pins.\n", ctx->nameOf(ci));
        }
        CellInfo *clk_src = clkin_net->driver.cell;
        if (!is_io(clk_src)) {
            log_error("Clock source for DLLDLY %s is not IO: %s.\n", ctx->nameOf(ci), ctx->nameOf(clk_src));
        }
        // DLLDLY placement is fixed
        BelId io_bel = clk_src->bel;
        BelId dlldly_bel = gwu.get_dlldly_bel(io_bel);
        if (dlldly_bel == BelId()) {
            log_error("Can't use IO %s as input for DLLDLY %s.\n", ctx->nameOf(clk_src), ctx->nameOf(ci));
        }
        if (ctx->verbose) {
            log_info("  pack %s to use clock pin at %s\n", ctx->nameOf(ci), ctx->nameOfBel(io_bel));
        }
        ctx->bindBel(dlldly_bel, ci, STRENGTH_LOCKED);
        gwu.remove_brackets(ci);
    }
}

// =========================================
// Create entry points to the clock system
// =========================================
void GowinPacker::pack_buffered_nets(void)
{
    log_info("Pack buffered nets...\n");

    for (auto &net : ctx->nets) {
        NetInfo *ni = net.second.get();
        if (ni->driver.cell == nullptr || ni->users.empty() || net.first == ctx->id("$PACKER_GND") ||
            net.first == ctx->id("$PACKER_VCC")) {
            continue;
        }
        if (ni->attrs.count(id_CLOCK) == 0) {
            if (ctx->settings.count(id_NO_GP_CLOCK_ROUTING)) {
                continue;
            }
            if (gwu.driver_is_clksrc(ni->driver) || (!gwu.driver_is_io(ni->driver))) {
                // no need for buffering
                continue;
            }
            // check users for the clock inputs
            bool has_clock_users = false;
            for (auto usr : ni->users) {
                if (usr.port.in(id_CLKIN, id_CLK, id_CLK0, id_CLK1, id_CLK2, id_CLK3, id_CLKFB)) {
                    // Latch gate signals drive CLK pins but are not clocks
                    if (usr.port == id_CLK && usr.cell->attrs.count(id_LATCH))
                        continue;
                    has_clock_users = true;
                    break;
                }
            }
            if (!has_clock_users) {
                continue;
            }
            if (ctx->verbose) {
                log_info("Add buffering to a potentially clock network '%s'\n", ctx->nameOf(ni));
            }
        }

        // make new BUF cell single user for the net driver
        IdString buf_name = ctx->idf("%s_BUFG", net.first.c_str(ctx));
        ctx->createCell(buf_name, id_BUFG);
        CellInfo *buf_ci = ctx->cells.at(buf_name).get();
        buf_ci->addInput(id_I);
        // move driver
        CellInfo *driver_cell = ni->driver.cell;
        IdString driver_port = ni->driver.port;

        driver_cell->movePortTo(driver_port, buf_ci, id_O);
        buf_ci->connectPorts(id_I, driver_cell, driver_port);
    }
}

// =========================================
// Create DQCEs
// =========================================
void GowinPacker::pack_dqce(void)
{
    log_info("Pack DQCE cells...\n");

    // At the placement stage, nothing can be said definitively about DQCE,
    // so we make user cells virtual but allocate all available bels by
    // creating and placing cells - we will use some of them after, and
    // delete the rest.
    // We do this here because the decision about which physical DQCEs to
    // use is made during routing, but some of the information (let’s say
    // mapping cell pins -> bel pins) is filled in before routing.
    // The GW5A family spells the primitive DCE, not DQCE (GowinSynthesis
    // answers EX3937 for DQCE; UG306-1.0.1E S3.1 names it DCE).  Only the
    // cell is renamed -- the ports are the same -- so normalise the two names
    // here instead of carrying a second cell type through the whole flow, the
    // same way DHCE is normalised onto DHCEN.
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;
        if (ci.type == id_DCE) {
            ci.type = id_DQCE;
        }
    }

    bool grab_bels = false;
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;
        if (ci.type == id_DQCE) {
            ci.pseudo_cell = std::make_unique<RegionPlug>(Loc(0, 0, 0));
            grab_bels = true;
        }
    }
    if (grab_bels) {
        for (int i = 0; i < 32; ++i) {
            BelId dqce_bel = gwu.get_dqce_bel(ctx->idf("SPINE%d", i));
            if (dqce_bel != BelId()) {
                IdString dqce_name = ctx->idf("$PACKER_DQCE_SPINE%d", i);
                CellInfo *dqce = ctx->createCell(dqce_name, id_DQCE);
                dqce->addInput(id_CE);
                ctx->bindBel(dqce_bel, dqce, STRENGTH_LOCKED);
            }
        }
    }
}

// =========================================
// Create DCSs
// =========================================
void GowinPacker::pack_dcs(void)
{
    log_info("Pack DCS cells...\n");

    // At the placement stage, nothing can be said definitively about DCS,
    // so we make user cells virtual but allocate all available bels by
    // creating and placing cells - we will use some of them after, and
    // delete the rest.
    // We do this here because the decision about which physical DCEs to
    // use is made during routing, but some of the information (let’s say
    // mapping cell pins -> bel pins) is filled in before routing.
    bool grab_bels = false;
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;
        if (ci.type == id_DCS) {
            ci.pseudo_cell = std::make_unique<RegionPlug>(Loc(0, 0, 0));
            grab_bels = true;
        }
    }
    if (grab_bels) {
        for (int i = 0; i < 8; ++i) {
            BelId dcs_bel = gwu.get_dcs_bel(ctx->idf("P%d%dA", 1 + (i % 4), 6 + (i >> 2)));
            if (dcs_bel != BelId()) {
                IdString dcs_name = ctx->idf("$PACKER_DCS_SPINE%d", 8 * (i % 4) + 6 + (i >> 2));
                CellInfo *dcs = ctx->createCell(dcs_name, id_DCS);
                ctx->copyBelPorts(dcs_name, dcs_bel);
                ctx->bindBel(dcs_bel, dcs, STRENGTH_LOCKED);
            }
        }
    }
}

// =========================================
// Create DHCENs
// =========================================
void GowinPacker::pack_dhcens(void)
{
    // The GW5A family spells this primitive DHCE and its enable port CEN --
    // DHCEN does not exist there at all (GowinSynthesis answers
    // "ERROR (EX3937) : Instantiating unknown module 'DHCEN'", and yosys' own
    // gowin/cells_xtra_gw5a.v carries DHCE(CLKIN, CEN, CLKOUT)).  The bel, the
    // packer below and globals.cc's route_dhcen_net are spelling-agnostic once
    // the cell is renamed, so normalise the two names here instead of carrying
    // a second cell type through the whole flow.
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;
        if (ci.type == id_DHCE) {
            ci.type = id_DHCEN;
            if (ci.ports.count(id_CEN)) {
                ci.renamePort(id_CEN, id_CE);
            }
        }
    }

    // Allocate all available dhcen bels; we will find out which of them
    // will actually be used during the routing process.
    bool grab_bels = false;
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;
        if (ci.type == id_DHCEN) {
            ci.pseudo_cell = std::make_unique<RegionPlug>(Loc(0, 0, 0));
            grab_bels = true;
        }
    }
    if (grab_bels) {
        // sane message if new primitives are used with old bases
        auto buckets = ctx->getBelBuckets();
        NPNR_ASSERT_MSG(std::find(buckets.begin(), buckets.end(), id_DHCEN) != buckets.end(),
                        "There are no DHCEN bels to use.");
        int i = 0;
        for (auto &bel : ctx->getBelsInBucket(ctx->getBelBucketForCellType(id_DHCEN))) {
            IdString dhcen_name = ctx->idf("$PACKER_DHCEN_%d", ++i);
            CellInfo *dhcen = ctx->createCell(dhcen_name, id_DHCEN);
            dhcen->addInput(id_CE);
            ctx->bindBel(bel, dhcen, STRENGTH_LOCKED);
        }
    }
}

// =========================================
// Enable UserFlash
// =========================================
void GowinPacker::pack_userflash(bool have_emcu)
{
    log_info("Pack UserFlash cells...\n");
    std::vector<std::unique_ptr<CellInfo>> new_cells;

    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;
        if (!is_userflash(&ci)) {
            continue;
        }

        if (ci.type.in(id_FLASH96K, id_FLASH256K, id_FLASH608K)) {
            // enable
            ci.addInput(id_INUSEN);
            ci.connectPort(id_INUSEN, ctx->nets.at(ctx->id("$PACKER_GND")).get());
        }
        gwu.remove_brackets(&ci);

        if (have_emcu) {
            continue;
        }

        // add invertor
        int lut_idx = 0;
        auto add_inv = [&](IdString port, PortType port_type) {
            if (!gwu.port_used(&ci, port)) {
                return;
            }

            std::unique_ptr<CellInfo> lut_cell =
                    gwu.create_cell(gwu.create_aux_name(ci.name, lut_idx, "_lut$"), id_LUT4);
            new_cells.push_back(std::move(lut_cell));
            CellInfo *lut = new_cells.back().get();
            lut->addInput(id_I0);
            lut->addOutput(id_F);
            lut->setParam(id_INIT, 0x5555);
            ++lut_idx;

            if (port_type == PORT_IN) {
                ci.movePortTo(port, lut, id_I0);
                lut->connectPorts(id_F, &ci, port);
            } else {
                ci.movePortTo(port, lut, id_F);
                ci.connectPorts(port, lut, id_I0);
            }
        };
        for (auto pin : ci.ports) {
            if (pin.second.type == PORT_OUT) {
                add_inv(pin.first, PORT_OUT);
            } else {
                if (pin.first == id_INUSEN) {
                    continue;
                }
                if (ci.type == id_FLASH608K && pin.first.in(id_XADR0, id_XADR1, id_XADR2, id_XADR3, id_XADR4, id_XADR5,
                                                            id_XADR6, id_XADR7, id_XADR8)) {
                    continue;
                }
                add_inv(pin.first, PORT_IN);
            }
        }
    }
    for (auto &ncell : new_cells) {
        ctx->cells[ncell->name] = std::move(ncell);
    }
}

// =========================================
// Create AE350_SOC
// =========================================
void GowinPacker::pack_ae350(void)
{
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;
        if (!is_ae350(&ci)) {
            continue;
        }
        // The chipdb names a bus bit `NAME<i>`, the netlist `NAME[i]`.
        gwu.remove_brackets(&ci);

        // A port the device data leaves unmapped has no bel pin, and so no
        // fabric wire to route to. The vendor's own tools accept such a port
        // and drop it, so refusing the design would refuse every AE350 netlist
        // written against the primitive; disconnect it instead, loudly, so the
        // gap is in the log and in the netlist rather than routed somewhere
        // plausible and wrong.
        pool<IdString> bel_pins;
        bool have_site = false;
        for (BelId bel : ctx->getBels()) {
            if (ctx->getBelType(bel) != id_AE350_SOC) {
                continue;
            }
            for (IdString pin : ctx->getBelPins(bel)) {
                bel_pins.insert(pin);
            }
            have_site = true;
            break;
        }
        // Without a site every port would look unmapped and be disconnected in
        // silence, which is the one outcome worse than refusing: the design
        // would build, and the SoC would simply not be wired to anything.
        if (!have_site) {
            log_error("Cell '%s' is an AE350_SOC, but this device has no AE350_SOC site.\n",
                      ci.name.c_str(ctx));
        }
        std::vector<IdString> unmapped;
        for (auto &port : ci.ports) {
            if (port.second.net != nullptr && !bel_pins.count(port.first)) {
                unmapped.push_back(port.first);
            }
        }
        for (IdString port : unmapped) {
            log_warning("AE350_SOC port %s is not mapped by the device data; disconnecting it.\n",
                        port.c_str(ctx));
            ci.disconnectPort(port);
        }
    }
}

// =========================================
// Create EMCU
// =========================================
void GowinPacker::pack_emcu_and_flash(void)
{
    log_info("Pack EMCU and UserFlash cells...\n");
    std::vector<std::unique_ptr<CellInfo>> new_cells;

    bool have_emcu = false;
    for (auto &cell : ctx->cells) {
        auto &ci = *cell.second;
        if (!is_emcu(&ci)) {
            continue;
        }
        have_emcu = true;

        gwu.remove_brackets(&ci);

        // The flash data bus is connected directly to the CPU so just disconnect these networks
        // also other non-switched networks
        ci.disconnectPort(ctx->id("DAPNTDOEN"));
        ci.disconnectPort(ctx->id("DAPNTRST"));
        ci.disconnectPort(ctx->id("DAPTDO"));
        ci.disconnectPort(ctx->id("DAPTDI"));
        ci.disconnectPort(ctx->id("TARGFLASH0HREADYMUX"));
        ci.disconnectPort(ctx->id("TARGEXP0HAUSER"));
        ci.disconnectPort(ctx->id("TARGFLASH0EXRESP"));
        ci.disconnectPort(ctx->id("PORESETN"));
        ci.disconnectPort(ctx->id("SYSRESETN"));
        ci.disconnectPort(ctx->id("DAPSWDITMS"));
        ci.disconnectPort(ctx->id("DAPSWCLKTCK"));
        ci.disconnectPort(ctx->id("TPIUTRACECLK"));
        for (int i = 0; i < 32; ++i) {
            if (i < 4) {
                if (i < 3) {
                    ci.disconnectPort(ctx->idf("TARGFLASH0HSIZE%d", i));
                    ci.disconnectPort(ctx->idf("TARGFLASH0HBURST%d", i));
                    ci.disconnectPort(ctx->idf("TARGFLASH0HRUSER%d", i));
                    ci.disconnectPort(ctx->idf("INITEXP0HRUSER%d", i));
                }
                // ci.disconnectPort(ctx->idf("TARGFLASH0HPROT%d", i));
                ci.disconnectPort(ctx->idf("TARGEXP0HWUSER%d", i));
                ci.disconnectPort(ctx->idf("MTXREMAP%d", i));
            }
            // ins
            ci.disconnectPort(ctx->idf("TARGFLASH0HRDATA%d", i));
        }
    }
    pack_userflash(have_emcu);
}

void GowinPacker::run(void)
{
    handle_constants();
    pack_iobs();
    ctx->check();

    pack_i3c();
    ctx->check();

    pack_mipi();
    ctx->check();

    pack_diff_iobs();
    ctx->check();

    pack_io_regs();
    ctx->check();

    pack_iodelay();
    ctx->check();

    pack_iem();
    ctx->check();

    pack_iologic();
    ctx->check();

    pack_dqs();
    ctx->check();

    pack_io16();
    ctx->check();

    pack_gsr();
    ctx->check();

    pack_pincfg();
    ctx->check();

    pack_hclk();
    ctx->check();

    pack_dlldly();
    ctx->check();

    pack_bandgap();
    ctx->check();

    pack_wideluts();
    ctx->check();

    pack_alus();
    ctx->check();

    pack_ssram();
    ctx->check();

    pack_latches();
    ctx->check();

    constrain_lutffs();
    ctx->check();

    pack_pll();
    ctx->check();

    pack_adc();
    ctx->check();

    pack_bsram();
    ctx->check();

    pack_dsp();
    ctx->check();

    pack_inv();
    ctx->check();

    pack_buffered_nets();
    ctx->check();

    pack_ae350();
    ctx->check();

    pack_emcu_and_flash();
    ctx->check();

    pack_dhcens();
    ctx->check();

    pack_dqce();
    ctx->check();

    pack_dcs();
    ctx->check();

    ctx->fixupHierarchy();
    ctx->check();
}

void gowin_pack(Context *ctx)
{
    GowinPacker packer(ctx);
    packer.run();
}

NEXTPNR_NAMESPACE_END
