#!/usr/bin/env python3
"""Gate check: the four measured GW5AST-138C DQS sites and their dedicated links.

The DQS is modelled from vendor measurements (TinyVCMI TOOL-007, TOOL-008,
TOOL-011 and TOOL-012), one site per DDR3 byte lane, and several things about
the model are easy to break without any build failing:

  test_dqs_sites_get_tile_types_of_their_own
      Lanes 0 and 3 share chipdb type 241, which appears eight times, and
      lane 1's type 257 appears three times.  `mark_dqs_site` must make every
      site cell's extra functions differ from every other cell of its type,
      including the other site, or the `create_tiletype` dedup copies a bel
      onto unmeasured cells.

  test_dqs_pin_wires_are_real_and_free
      At every site, every input wire is a pip destination and every output
      wire a pip source; no other bel already sits on any of them; no wire is
      used for two pins.

  test_dedicated_link_sources_have_one_driver
      DQSIN, DLLSTEP and LOCK are joined to their source wires with no pip,
      which is safe only while nothing else can drive those wires: each
      lane's F6 is driven by its strobe pad's IOBA.O alone, and Q0..Q7 at
      X1Y108 and Q7 at X1Y99 carry no bel pin in the database.

  test_lane_strobe_pads_match_their_sources
      Each lane's strobe pad is an IOLnA pad whose cell is the lane's DQSIN
      source and its first IOLOGIC position.

  test_every_fclk_lane_reaches_every_site
      Each of the four block-2 lanes is driven from its `HCLK_MUX_ALPHA2<n>`
      and, after `extend_dqs_fclk_lanes`, has every site in its node.

  test_a_link_onto_a_node_wire_is_refused
      `create_dedicated_links` refuses a wire that already belongs to a node,
      because a tile wire can belong to one node only.

  test_dq_lane_positions_are_iologic_of_marked_tiles
      Every lane position names an IOLOGIC bel the database has, no position
      belongs to two lanes, every tile is marked so the memory-mode pins reach
      no other cell of its type, and every DQS output a lane link starts from
      is a DQS bel output.

Run it with Apicula on the path, as the generator itself needs it:

    source tools/openflow_env.sh && python3 tests/check_dqs_site.py
"""
import copy
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(HERE, ".."))

from himbaechel_dbgen.chip import Chip  # noqa: E402
import gowin_arch_gen as gen  # noqa: E402

DEVICE = "GW5AST-138C"
_DB = None


def _db():
    global _DB
    if _DB is None:
        import importlib.resources as ir
        from apycula.chipdb import load_chipdb
        _DB = load_chipdb(str(ir.files("apycula") / f"{DEVICE}.msgpack.xz"))
    return _DB


def _bel_pins_on(db, x, y, wire):
    return [f"{name}.{port}" for name, bel in db[y, x].bels.items()
            for port, w in getattr(bel, "portmap", {}).items() if w == wire]


def test_dqs_sites_get_tile_types_of_their_own():
    db = copy.copy(_db())
    db.extra_func = copy.deepcopy(db.extra_func)
    counts = {}
    for x, y in gen.DQS_SITES:
        ttyp = db.grid[y][x]
        counts[ttyp] = sum(row.count(ttyp) for row in db.grid)
    assert counts == {241: 8, 257: 3, 259: 1}, f"site type counts changed: {counts}"
    gen.mark_dqs_site(db)
    for x, y in gen.DQS_SITES:
        ttyp = db.grid[y][x]
        mine = db.extra_func.get((y, x))
        for r in range(db.rows):
            for c in range(db.cols):
                if db.grid[r][c] == ttyp and (r, c) != (y, x):
                    assert db.extra_func.get((r, c)) != mine, \
                        f"{(r, c)} would share the DQS tile type of X{x}Y{y}"


def test_dqs_pin_wires_are_real_and_free():
    db = _db()
    wires = list(gen.DQS_INPUTS.values()) + list(gen.DQS_OUTPUTS.values())
    assert len(set(wires)) == len(wires), "a wire is used for two DQS pins"
    for x, y in gen.DQS_SITES:
        pips = db[y, x].pips
        sources = {s for srcs in pips.values() for s in srcs}
        for pin, wire in gen.DQS_INPUTS.items():
            assert wire in pips, f"X{x}Y{y} input {pin} on {wire}: not a pip destination"
        for pin, wire in gen.DQS_OUTPUTS.items():
            assert wire in sources, f"X{x}Y{y} output {pin} on {wire}: not a pip source"
        for wire in wires:
            assert not _bel_pins_on(db, x, y, wire), f"X{x}Y{y}/{wire} already carries a bel pin"


def test_dedicated_link_sources_have_one_driver():
    db = _db()
    for lane in gen.DQS_LANES:
        x, y, wire = lane.dqsin
        assert sorted(_bel_pins_on(db, x, y, wire)) in (["IOBA.O"], ["IOBA.O", "IOLOGICA.D"]), \
            f"DQSIN source X{x}Y{y}/{wire} carries {_bel_pins_on(db, x, y, wire)}"
        assert wire not in db[y, x].pips, f"X{x}Y{y}/{wire} is driven by a pip"
    for x, y, wire in gen.DQS_DLLSTEP_SOURCE + [gen.DDRDLL_LOCK_TAP]:
        assert not _bel_pins_on(db, x, y, wire), \
            f"X{x}Y{y}/{wire} carries {_bel_pins_on(db, x, y, wire)}"
        assert wire not in db[y, x].pips, f"X{x}Y{y}/{wire} is driven by a pip"


def test_lane_strobe_pads_match_their_sources():
    db = _db()
    pinout = db.pinout[DEVICE]["PBGA484A"]
    for lane in gen.DQS_LANES:
        io = pinout[lane.pad][0]
        assert io.startswith("IOL") and io.endswith("A"), f"{lane.pad} is {io}"
        cell = (0, int(io[3:-1]) - 1)
        assert lane.dqsin[:2] == cell, f"{lane.pad}: DQSIN source {lane.dqsin} is not cell {cell}"
        assert lane.iologic[0] == cell + ("A",), f"{lane.pad}: first position {lane.iologic[0]}"


def test_every_fclk_lane_reaches_every_site():
    db = copy.copy(_db())
    db.nodes = copy.deepcopy(db.nodes)
    gen.extend_dqs_fclk_lanes(db)
    for n, lane in enumerate(gen.DQS_FCLK_LANES):
        drivers = db.hclk_pips.get((0, 0), {}).get(lane, {})
        assert f"HCLK_MUX_ALPHA2{n}" in drivers, f"{lane} is not driven from its ALPHA mux"
        members = {tuple(m) for m in db.nodes[f"HCLK2_{lane}"][1]}
        for x, y in gen.DQS_SITES:
            assert (y, x, lane) in members, f"{lane} does not reach the DQS site X{x}Y{y}"


class _FakeDb:
    def __init__(self, rows, nodes):
        self.rows = rows
        self.nodes = nodes


def test_a_link_onto_a_node_wire_is_refused():
    chip = Chip("gowin", "TEST", 8, 110)
    tt = chip.create_tile_type("EMPTY")
    names = {"F6", "Q7", "DDRDLL_LOCK", "DQS_DQSIN"}
    names |= {f"Q{i}" for i in range(8)} | {f"DQS_DLLSTEP{i}" for i in range(8)}
    names |= {gen.DQS_OUTPUTS[out] for out in gen.DQ_LANE_LINKS}
    names |= {gen.dq_link_wire(side, pin) for side in "AB"
              for sinks in gen.DQ_LANE_LINKS.values() for _, pin in sinks}
    for name in sorted(names):
        tt.create_wire(name)
    for yy in range(110):
        for xx in range(8):
            chip.set_tile_type(xx, yy, "EMPTY")
    x, y, wire = gen.DQS_LANES[0].dqsin
    taken = _FakeDb(109, {"N": ("", {(y, x, wire), (0, 0, wire)})})
    try:
        gen.create_dedicated_links(chip, taken)
    except ValueError:
        pass
    else:
        raise AssertionError("a link onto a wire already in a node was accepted")
    gen.create_dedicated_links(chip, _FakeDb(109, {}))


def test_dq_lane_positions_are_iologic_of_marked_tiles():
    db = copy.copy(_db())
    db.extra_func = copy.deepcopy(db.extra_func)
    gen.mark_dqs_site(db)
    positions = [p for lane in gen.DQS_LANES for p in lane.iologic]
    assert len(positions) == 40 and len(set(positions)) == 40, "lane positions overlap"
    for x, y, side in positions:
        assert f"IOLOGIC{side}" in db[y, x].bels, f"X{x}Y{y} has no IOLOGIC{side}"
        assert "tinyvcmi_dq_lane" in db.extra_func.get((y, x), {}), f"X{x}Y{y} is not marked"
    assert not gen.DQS_SITES & set(gen.DQ_LANE_TILES), "a DQS site is also a lane IO cell"
    marked = set(gen.DQ_LANE_TILES)
    for ttyp in {db.grid[y][x] for x, y in marked}:
        others = [(r, c) for r in range(db.rows) for c in range(db.cols)
                  if db.grid[r][c] == ttyp and (c, r) not in marked]
        for cell in others:
            assert "tinyvcmi_dq_lane" not in db.extra_func.get(cell, {}), f"{cell} is marked"
    for out in gen.DQ_LANE_LINKS:
        assert out in gen.DQS_OUTPUTS, f"lane link source {out} is not a DQS output"


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
            except AssertionError as exc:
                failures += 1
                print(f"FAIL {name}: {exc}")
            else:
                print(f"ok   {name}")
    sys.exit(1 if failures else 0)
