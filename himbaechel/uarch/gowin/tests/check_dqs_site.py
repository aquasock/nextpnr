#!/usr/bin/env python3
"""Gate check: the one measured GW5AST-138C DQS site and its dedicated links.

The DQS is modelled from vendor measurements (TinyVCMI TOOL-007 and TOOL-008),
and three things about the model are easy to break without any build failing:

  test_dqs_site_gets_a_tile_type_of_its_own
      Its chipdb tile type appears eight times.  `mark_dqs_site` must make
      that one cell's extra functions differ from the other seven, or the
      `create_tiletype` dedup copies the bel onto seven unmeasured cells.

  test_dqs_pin_wires_are_real_and_free
      Every input wire is a pip destination of the site tile and every output
      wire a pip source; no other bel already sits on any of them; no wire is
      used for two pins.

  test_dedicated_link_sources_have_one_driver
      DQSIN, DLLSTEP and LOCK are joined to their source wires with no pip,
      which is safe only while nothing else can drive those wires: F6 at
      X0Y68 is driven by IOBA.O alone, and Q0..Q7 at X1Y108 and Q7 at X1Y99
      carry no bel pin in the database.

  test_every_fclk_lane_reaches_the_site
      Each of the four block-2 lanes is driven from its `HCLK_MUX_ALPHA2<n>`
      and, after `extend_dqs_fclk_lanes`, has the site in its node.

  test_a_link_onto_a_node_wire_is_refused
      `create_dedicated_links` refuses a wire that already belongs to a node,
      because a tile wire can belong to one node only.

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


def test_dqs_site_gets_a_tile_type_of_its_own():
    db = copy.copy(_db())
    db.extra_func = copy.deepcopy(db.extra_func)
    x, y = gen.DQS_SITE
    ttyp = db.grid[y][x]
    others = [(r, c) for r in range(db.rows) for c in range(db.cols)
              if db.grid[r][c] == ttyp and (r, c) != (y, x)]
    assert len(others) == 7, f"expected 7 other cells of type {ttyp}, got {len(others)}"
    gen.mark_dqs_site(db)
    mine = db.extra_func.get((y, x))
    for cell in others:
        assert db.extra_func.get(cell) != mine, f"{cell} would share the DQS tile type"


def test_dqs_pin_wires_are_real_and_free():
    db = _db()
    x, y = gen.DQS_SITE
    pips = db[y, x].pips
    sources = {s for srcs in pips.values() for s in srcs}
    for pin, wire in gen.DQS_INPUTS.items():
        assert wire in pips, f"input {pin} on {wire}: not a pip destination"
    for pin, wire in gen.DQS_OUTPUTS.items():
        assert wire in sources, f"output {pin} on {wire}: not a pip source"
    wires = list(gen.DQS_INPUTS.values()) + list(gen.DQS_OUTPUTS.values())
    assert len(set(wires)) == len(wires), "a wire is used for two DQS pins"
    for wire in wires:
        assert not _bel_pins_on(db, x, y, wire), f"{wire} already carries a bel pin"


def test_dedicated_link_sources_have_one_driver():
    db = _db()
    x, y, wire = gen.DQS_DQSIN_SOURCE
    assert _bel_pins_on(db, x, y, wire) == ["IOBA.O"] or \
        sorted(_bel_pins_on(db, x, y, wire)) == ["IOBA.O", "IOLOGICA.D"], \
        f"DQSIN source X{x}Y{y}/{wire} carries {_bel_pins_on(db, x, y, wire)}"
    for x, y, wire in gen.DQS_DLLSTEP_SOURCE + [gen.DDRDLL_LOCK_TAP]:
        assert not _bel_pins_on(db, x, y, wire), \
            f"X{x}Y{y}/{wire} carries {_bel_pins_on(db, x, y, wire)}"
        assert wire not in db[y, x].pips, f"X{x}Y{y}/{wire} is driven by a pip"


def test_every_fclk_lane_reaches_the_site():
    db = copy.copy(_db())
    db.nodes = copy.deepcopy(db.nodes)
    gen.extend_dqs_fclk_lanes(db)
    x, y = gen.DQS_SITE
    for n, lane in enumerate(gen.DQS_FCLK_LANES):
        drivers = db.hclk_pips.get((0, 0), {}).get(lane, {})
        assert f"HCLK_MUX_ALPHA2{n}" in drivers, f"{lane} is not driven from its ALPHA mux"
        members = {tuple(m) for m in db.nodes[f"HCLK2_{lane}"][1]}
        assert (y, x, lane) in members, f"{lane} does not reach the DQS site"


class _FakeDb:
    def __init__(self, rows, nodes):
        self.rows = rows
        self.nodes = nodes


def test_a_link_onto_a_node_wire_is_refused():
    chip = Chip("gowin", "TEST", 8, 110)
    tt = chip.create_tile_type("EMPTY")
    for name in ("F6", "Q7", "DDRDLL_LOCK", "DQS_DQSIN"):
        tt.create_wire(name)
    for i in range(8):
        tt.create_wire(f"Q{i}")
        tt.create_wire(f"DQS_DLLSTEP{i}")
    for yy in range(110):
        for xx in range(8):
            chip.set_tile_type(xx, yy, "EMPTY")
    x, y, wire = gen.DQS_DQSIN_SOURCE
    taken = _FakeDb(109, {"N": ("", {(y, x, wire), (0, 0, wire)})})
    try:
        gen.create_dedicated_links(chip, taken)
    except ValueError:
        pass
    else:
        raise AssertionError("a link onto a wire already in a node was accepted")
    gen.create_dedicated_links(chip, _FakeDb(109, {}))


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
