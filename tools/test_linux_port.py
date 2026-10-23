"""Tests for the native Linux build tooling (port/linux, tools/linux_*.py)."""

import math
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

import pytest

from tools import linux_build, linux_link_check, linux_msvc_semantics


# ---------- MSVC semantics header


def test_strip_cplusplus_keeps_c_branches_only():
    text = "\n".join([
        "a",
        "#ifdef __cplusplus", "cpp1", "#else", "c1", "#endif",
        "#if FOO", "b", "#else", "c", "#endif",
        "#ifndef __cplusplus", "c2", "#else", "cpp2", "#endif",
        "#if defined(__cplusplus)", "cpp3", "#if X", "cpp4", "#endif", "#endif",
        "#ifdef _WIN64", "win64", "#endif",
        "end",
    ])
    assert linux_msvc_semantics.strip_cplusplus(text).split() == ["a", "c1", "b", "c", "c2", "end"]


def write(path: Path, text: str) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="latin-1")
    return path


def test_scan_finds_prototype_scope_tags_and_inline_functions(tmp_path):
    header = write(tmp_path / "a.h", """
        struct location;
        void f(struct scenario *s, union color *c);
        /* struct commented_out */
        __inline long prototyped(long x) { return x; }
        __inline long private_helper(long x) { return x; }
        long prototyped(long x);
        #ifdef __cplusplus
        __inline long cpp_only(long x) { return x; }
        #endif
    """)
    files = [header]
    tags = linux_msvc_semantics.scan_tags(files)
    assert tags == {"location": {"struct"}, "scenario": {"struct"}, "color": {"union"}}
    assert linux_msvc_semantics.scan_inline_functions(files, all_inlines=False) == {"prototyped"}
    assert linux_msvc_semantics.scan_inline_functions(files, all_inlines=True) == {
        "prototyped", "private_helper",
    }


def test_render_skips_tags_used_as_both_struct_and_union():
    text = linux_msvc_semantics.render(
        {"a": {"struct"}, "b": {"union"}, "mixed": {"struct", "union"}}, {"f"}
    )
    assert "struct a;" in text
    assert "union b;" in text
    assert "mixed" not in text
    assert "#pragma weak f" in text


# ---------- Xbox SDK declarations (port/include/xdk)


XDK_INCLUDE = Path(__file__).resolve().parent.parent / "port" / "include" / "xdk"


def test_xdk_headers_use_the_sdk_spellings():
    # each port gives the SDK's keywords and names its own meaning (its
    # prefix header), so none of one port's must be written into them
    for header in sorted(XDK_INCLUDE.glob("*.h")):
        text = re.sub(r"/\*.*?\*/", "", header.read_text(encoding="utf-8"), flags=re.S)
        assert "__attribute__" not in text, header.name
        assert not re.findall(r"\bhalo_\w+", text), header.name


@pytest.mark.skipif(shutil.which("clang") is None, reason="clang is needed to compile the headers")
def test_xdk_headers_compile_for_the_game(tmp_path):
    root = XDK_INCLUDE.parent.parent.parent
    source = write(tmp_path / "unit.c", "".join(
        f"#include <{name}>\n" for name in ("xtl.h", "xbdm.h", "xkbd.h", "d3d8perf.h")
    ))
    flags = [flag for flag in linux_build.LINUX_ABI_FLAGS + linux_build.GAME_FLAGS if flag != "-w"]
    for defines in ([], ["-DDEBUG_KEYBOARD"], ["-DNOD3D", "-DNODSOUND"]):
        subprocess.run(
            ["clang", *flags, *defines, "-Werror", "-fsyntax-only",
             "-include", str(root / "port/linux/include/halo_linux_prefix.h"),
             "-I", str(root / "port/linux/include"), "-idirafter", str(XDK_INCLUDE), str(source)],
            check=True,
        )


def pdb_writer():
    """An xdk_headers Writer over an empty type table: basic types only."""
    from tools import pdb200_types, xdk_headers
    pdb = object.__new__(pdb200_types.Pdb)
    pdb.types, pdb.aggregates = {}, {}
    return xdk_headers, xdk_headers.Writer(pdb)


def test_xdk_headers_regroups_anonymous_members():
    # the PDB lists an anonymous structure's or union's members in their
    # container, at their offsets
    xdk_headers, writer = pdb_writer()
    member = xdk_headers.Member
    ulong, int64 = 0x22, 0x13
    union = writer.render(writer.group(
        [member("LowPart", ulong, 0), member("HighPart", ulong, 4), member("QuadPart", int64, 0)], True), "", "u")
    assert union == ["struct {", "    unsigned long LowPart;", "    unsigned long HighPart;", "};",
                     "__int64 QuadPart;"]
    structure = writer.render(writer.group(
        [member("Tag", ulong, 0), member("Low", ulong, 4), member("High", ulong, 8),
         member("Whole", int64, 4), member("After", ulong, 12)], False), "", "s")
    assert structure == ["unsigned long Tag;", "union {", "    struct {", "        unsigned long Low;",
                         "        unsigned long High;", "    };", "    __int64 Whole;", "};",
                         "unsigned long After;"]


# ---------- weak reference link check


@pytest.mark.skipif(shutil.which("clang") is None or shutil.which("nm") is None,
                    reason="clang and nm are needed to build test objects")
def test_link_check_rejects_undefined_weak_references(tmp_path):
    def compile_object(name: str, text: str) -> Path:
        source = write(tmp_path / f"{name}.c", text)
        output = tmp_path / f"{name}.o"
        subprocess.run(["clang", "-c", "-o", str(output), str(source)], check=True)
        return output

    caller = compile_object("caller", "#pragma weak helper\nint helper(int);\nint call(void) { return helper(1); }\n")
    local = compile_object("local", "static int helper(int x) { return x; }\nint use(void) { return helper(2); }\n")
    provider = compile_object("provider", "#pragma weak helper\nint helper(int x) { return x; }\n")

    def check(*objects: Path) -> int:
        response = write(tmp_path / "objects.rsp", "\n".join(f"'{o}'" for o in objects))
        return subprocess.run(
            [sys.executable, linux_link_check.__file__, str(response)], capture_output=True
        ).returncode

    # a file-local copy elsewhere does not satisfy the reference
    assert check(caller, local) == 1
    assert check(caller, local, provider) == 0


# ---------- game sources


def test_game_sources_leave_out_the_excluded(tmp_path, monkeypatch):
    write(tmp_path / "source" / "a.c", "")
    write(tmp_path / "source" / "zlib" / "b.c", "")
    write(tmp_path / "source" / "zlib" / "example.c", "")
    write(tmp_path / "source" / "tiff" / "c.c", "")
    write(tmp_path / "source" / "tiff" / "deep" / "d.c", "")
    write(tmp_path / "source" / "tiffx.c", "")
    write(tmp_path / "source" / "a.h", "")
    monkeypatch.chdir(tmp_path)
    # (a folder is a name ending in "/": not a file that only starts like it)
    config = {"game": {"root": "source", "exclude": ["source/zlib/example.c", "source/tiff/"],
                       "defines": ["DEBUG"], "include_dirs": ["source", "source/saved games"]}}
    assert linux_build.game_sources(config) == [Path("source/a.c"), Path("source/tiffx.c"), Path("source/zlib/b.c")]
    assert linux_build.game_defines_and_includes(config) == '-DDEBUG -Isource -I"source/saved games"'


# ---------- menus (port/assets/menus)


MENUS = Path(__file__).resolve().parent.parent / "port/assets/menus"
MENU_ATTRIBUTES = {
    "menus": {"root"},
    "bitmap": {"name", "width", "height", "frames", "platform"},
    "frame": {"png", "map", "index", "width", "height", "x", "y", "platform"},
    "strings": {"name", "platform"},
    "string": {"text", "platform"},
    "widget": {"name", "type", "controller", "flags", "bitmap", "text", "strings", "values", "setting", "font",
               "color", "align", "description", "string_list", "list_flags", "text_flags", "header_bitmap",
               "footer_bitmap", "header_bounds", "footer_bounds", "child_controller", "x", "y", "left", "top",
               "width", "height", "text_x", "text_y", "auto_close", "auto_close_fade", "string_index", "platform"},
    "child": {"widget", "controller", "x", "y", "platform"},
    "on": {"event", "run", "script", "open", "replace", "close", "widget", "focus", "reload", "sound", "otherwise",
           "label", "back", "branch", "platform"},
    "data": {"input", "platform"},
    "conditional": {"widget", "if_failed", "platform"},
    "replace": {"search", "function", "platform"},
}


def c_strings(path: Path, start: str, end: str) -> list:
    """the string literals of a C file between two markers"""
    text = path.read_text(encoding="latin-1")
    text = text[text.index(start):]
    return re.findall(r'"([^"\\]*)"', text[:text.index(end)])


# menu_files.c's attributes read as whole numbers (its tables' { name, NULL, &long })
MENU_INTEGER_ATTRIBUTES = {"auto_close", "auto_close_fade", "height", "index", "left", "string_index", "text_x",
                           "text_y", "top", "width", "x", "y"}


def test_menus_are_well_formed():
    """What port/linux/src/menu_files.c and port/linux/game/menu_tags.c check
    when the game loads them, but the map's own names (paths with backslashes),
    which only the map has."""
    import json
    import xml.etree.ElementTree as ElementTree

    root = MENUS.parent.parent.parent
    tags = root / "port/linux/game/menu_tags.c"
    events = c_strings(tags, "event_names[] =", "};")
    flags = c_strings(tags, "widget_flag_names[] =", "};")
    functions = set(c_strings(root / "source/interface/ui_widget_event_handler_functions.c", '\t{\n\t\t"NULL",', "}"))
    functions |= set(c_strings(tags, "port_function_names[] =", "};"))
    inputs = set(c_strings(tags, "game_data_input_names[] =", "};"))
    inputs |= set(c_strings(tags, "port_game_data_input_names[] =", "};"))
    listed = json.loads((MENUS / "menus.json").read_text())["files"]
    files = sorted(MENUS.rglob("*.xml"))
    assert sorted(path.relative_to(MENUS).as_posix() for path in files) == \
        sorted(name for name in listed if name.endswith(".xml"))
    widgets, bitmaps, strings, references, roots = {}, {}, {}, [], []
    for path in files:
        tree = ElementTree.parse(path)
        assert tree.getroot().tag == "menus", path
        if tree.getroot().get("root"):
            roots.append(tree.getroot().get("root"))
        for element in tree.getroot().iter():
            where = f"{path.name}: <{element.tag} {element.get('name', '')}>"
            assert element.tag in MENU_ATTRIBUTES, where
            assert set(element.attrib) <= MENU_ATTRIBUTES[element.tag], where
            # (the platforms that show it: menu_files.c's MENU_PLATFORM)
            assert element.get("platform") is None or \
                set(element.get("platform").split()) <= {"desktop", "android", "web"}, where
            # (menu_files.c's whole numbers, which the tags keep in shorts,
            # its true/false attributes, and no text but whitespace outside
            # <string>s)
            for attribute in MENU_INTEGER_ATTRIBUTES & set(element.attrib):
                value = element.get(attribute)
                assert re.fullmatch(r"-?[0-9]+", value) and -32768 <= int(value) <= 32767, f"{where} {attribute}"
            for attribute in ("back", "branch", "if_failed"):
                assert element.get(attribute) in (None, "true", "false"), f"{where} {attribute}"
            assert not (element.text or "").strip() and not (element.tail or "").strip(), where
            if element.tag == "bitmap":
                bitmaps[element.get("name")] = element
                for frame in element.iter("frame"):
                    assert (frame.get("png") is None) != (frame.get("map") is None), where
                    if frame.get("map"):
                        assert "\\" in frame.get("map") and int(frame.get("index")) >= 0, where
                        continue
                    assert frame.get("png") in listed, where
                    with (MENUS / frame.get("png")).open("rb") as png:
                        header = png.read(24)
                    width, height = int.from_bytes(header[16:20], "big"), int.from_bytes(header[20:24], "big")
                    logical = int(frame.get("width")), int(frame.get("height"))
                    assert width % logical[0] == 0 and width // logical[0] == height // logical[1], where
            elif element.tag == "strings":
                strings[element.get("name")] = element
            elif element.tag == "widget":
                name = element.get("name")
                assert name and name not in widgets, where
                widgets[name] = element
                assert element.get("type", "container") in ("container", "text", "spinner", "column_list"), where
                assert set(element.get("flags", "").split()) <= set(flags), where
                children = element.findall("widget") + element.findall("child")
                if element.get("type") == "spinner":
                    assert len(children) in (0, 1, 3), where
                if element.get("strings") or "items_from_strings" in element.get("list_flags", ""):
                    assert element.get("type") == "spinner" and not children, where
                if element.get("setting"):
                    assert len(element.get("strings").split("|")) == len(element.get("values").split("|")), where
                for attribute in ("bitmap", "header_bitmap", "footer_bitmap"):
                    references.append((where, bitmaps, element.get(attribute)))
                references.append((where, strings, element.get("string_list")))
                references.append((where, widgets, element.get("description")))
            elif element.tag == "child" or element.tag == "conditional":
                references.append((where, widgets, element.get("widget")))
            elif element.tag == "on":
                assert set(element.get("event").split()) <= set(events), where
                run = element.get("run")
                assert run is None or run in functions or run.startswith("unwired "), where
                assert element.get("otherwise") is None or run, where
                for attribute in ("open", "replace", "focus", "widget", "otherwise"):
                    references.append((where, widgets, element.get(attribute)))
            elif element.tag == "data":
                assert element.get("input") in inputs or element.get("input").startswith("unwired "), where
    assert all(root_name in widgets for root_name in roots)
    for where, names, name in references:
        if name and "\\" not in name:
            assert name in names, f"{where} {name}"


def test_menu_settings_exist():
    """Every setting a menu's spinner sets is one of config.toml's
    (port/linux/src/port_config.c) or of the profile (menu_functions.c), and
    the keyboard's controls are the same in the settings, in the controls'
    screen and in the input code."""
    root = MENUS.parent.parent.parent
    config = (root / "port/linux/src/port_config.c").read_text()
    functions = (root / "port/linux/game/menu_functions.c").read_text()
    known = set(re.findall(r'^\t\{ "([a-z_]+\.[a-z_]+)", _config_', config, re.M))
    profile = set(re.findall(r'\{ "(profile\.[a-z_]+)", \d', functions))
    for path in (MENUS / "ce").glob("*.xml"):
        for setting in re.findall(r'setting="([^"]+)"', path.read_text()):
            assert setting in known | profile, f"{path.name}: {setting}"
    controls = set(re.findall(r'"(controls\.[a-z_]+)"', (root / "port/linux/src/xinput_sdl.c").read_text()))
    assert controls == {name for name in known if name.startswith("controls.")}
    assert controls == set(re.findall(r'\{ "(controls\.[a-z_]+)", L"', functions))


def test_p2p_signatures_and_listings(tmp_path):
    """internet play's Ed25519 (RFC 8032), the X25519 key of a seed, and the
    server browser's listings from host to browser (tools/p2p_lobby_check.c),
    built with the flags ninja gives the platform layer"""
    import shlex

    if not shutil.which("clang") or not shutil.which("ninja") or not Path("build.ninja").is_file():
        pytest.skip("needs clang, ninja and a configured build")
    command = subprocess.run(["ninja", "-t", "commands", "build/linux/obj/port/linux/src/p2p_crypto.o"],
                             capture_output=True, text=True, check=True).stdout.strip().splitlines()[-1]
    words = shlex.split(command)
    # (the compiler, and whatever runs it, ccache in CI: up to the first flag)
    while words and not words[0].startswith("-"):
        words = words[1:]
    flags = []
    skip = False
    for word in words:
        if skip:
            skip = False
        elif word in ("-MF", "-o", "-c"):
            skip = True
        elif word == "-MMD" or word.startswith("-flto") or word.startswith("-fprofile-use"):
            continue
        else:
            flags.append(word)
    program = tmp_path / "p2p_lobby_check"
    built = subprocess.run(["clang", *flags, "-O1", "-no-pie", "-Wl,--unresolved-symbols=ignore-all", "-o",
                            str(program), "tools/p2p_lobby_check.c", "port/linux/src/p2p_crypto.c",
                            "port/linux/src/p2p_lobby.c", "port/third_party/monocypher/monocypher.c",
                            "port/third_party/monocypher/monocypher-ed25519.c"],
                           capture_output=True, text=True)
    assert built.returncode == 0, built.stderr[-4000:]
    result = subprocess.run([str(program)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout
    assert "PASS" in result.stdout



# port_config.c on its own, with the app's file paths (HALO_DATA_ROOT) and C
# library calls: "get <setting>" prints it, "write <setting> <value>" sets it
CONFIG_PROGRAM = r"""
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "port_config.h"

void platform_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    vfprintf(stderr, format, arguments);
    va_end(arguments);
    fputc('\n', stderr);
}

int main(int argc, char **argv)
{
    char text[256];

    if (argc == 3 && !strcmp(argv[1], "get") && config_text(argv[2], text, sizeof(text)))
        return puts(text) < 0;
    if (argc == 4 && !strcmp(argv[1], "write"))
        return !config_write(argv[2], argv[3]);
    return 1;
}
"""


def test_config_has_each_section_once(tmp_path):
    """config.toml names each section once, as TOML asks: the defaults the
    game writes, and a file an older version wrote with [display] twice
    (which went unread), joined"""
    compiler = shutil.which("clang") or shutil.which("cc")
    if not compiler:
        pytest.skip("needs a C compiler")
    root = Path(__file__).resolve().parent.parent
    stubs = tmp_path / "stubs"
    (stubs / "SDL3").mkdir(parents=True)
    (stubs / "SDL3" / "SDL.h").write_text("")
    (stubs / "platform.h").write_text("void platform_log(const char *format, ...);\n")
    # (beside the stubs, which it then includes instead of the platform layer's)
    for name in ("port_config.c", "port_config.h"):
        shutil.copy(root / "port/linux/src" / name, stubs / name)
    (tmp_path / "main.c").write_text(CONFIG_PROGRAM)
    program = tmp_path / "config_program"
    subprocess.run([compiler, "-DHALO_ANDROID=1", "-DHALO_ARM64_GUEST=1", f"-I{stubs}",
                    f"-I{root}/port/third_party/tomlc17", "-o", str(program),
                    str(tmp_path / "main.c"), str(stubs / "port_config.c"),
                    str(root / "port/third_party/tomlc17/tomlc17.c"), "-lpthread"], check=True)
    data = tmp_path / "data"
    data.mkdir()
    config = data / "config.toml"

    def run(*command):
        result = subprocess.run([str(program), *command], capture_output=True, text=True,
                                env={**os.environ, "HALO_DATA_ROOT": str(data)})
        assert result.returncode == 0, result.stderr
        assert "using the defaults" not in result.stderr, result.stderr
        return result.stdout.strip(), result.stderr

    def sections():
        return re.findall(r"^\[(\w+)\]$", config.read_text(), re.M)

    # the defaults
    assert run("get", "display.first_person_legs")[0] == "false"
    assert len(sections()) == len(set(sections())), sections()
    run("write", "display.first_person_legs", "true")
    assert run("get", "display.first_person_legs")[0] == "true"

    # an older version's
    config.write_text("[display]\nvsync = false\n\n[game]\nconsole_log = \"all\"\n\n"
                      "[display]\nfirst_person_legs = true\n")
    value, log = run("get", "display.first_person_legs")
    assert value == "true" and "joined" in log
    assert sections().count("display") == 1
    assert run("get", "display.vsync")[0] == "false"
    assert run("get", "game.console_log")[0] == "all"


# ---------- system link's stand-in machines (tools/system_link_bots.py)


def test_system_link_bots_messages_are_the_games():
    """the stand-in machines number their messages as network_messages.h does,
    and send each at the size network_messages.c's fields decode (pad 0,
    bytes 1, shorts 2, longs 4; an array a count byte and one element)"""
    import struct
    import time
    import types
    from tools import system_link_bots as bots

    root = Path(__file__).resolve().parents[1]
    header = (root / "source/networking/network_messages.h").read_text()
    enum = header[header.index("enum network_game_message_type"):]
    names = re.findall(r"\t_message_(\w+)", enum[:enum.index("};")])
    for name, value in vars(bots).items():
        if name.lower() in names:
            assert names.index(name.lower()) == value, name
    sizes = {}
    source = (root / "source/networking/network_messages.c").read_text()
    for fields, name in re.findall(r"\t\{\n((?:\t\t.*\n)*?)\t\},\n\tNETWORK_GAME_MESSAGE_DEFINITION\((\w+),", source):
        sizes[name] = sum(int(count) * {"pad": 0, "bytes": 1, "shorts": 2, "longs": 4}.get(kind, 0) + (kind == "array")
                          for kind, count in re.findall(r"_data_packet_field_(\w+), (\d+)", fields))

    sent = []
    machine = bots.Machine(1, "127.0.0.2", "127.0.0.1", print)
    machine.udp.close()
    machine.send = sent.append
    machine.udp = types.SimpleNamespace(sendto=lambda data, address: sent.append(data))
    machine.joined()
    machine.handle(bots.SERVER_MACHINE_ACCEPTED, struct.pack(">ih", 0, 1))
    settings = bytes(0x24) + b"levels\\test\\bloodgulch\\bloodgulch".ljust(0x80, b"\0")
    machine.handle(bots.SERVER_GAME_SETTINGS_UPDATE, struct.pack(">HHH", len(settings), 0, len(settings)) + settings)
    machine.tick(time.monotonic())
    machine.handle(bots.SERVER_BEGIN_GAME, b"")
    machine.tick(time.monotonic() + 60)
    machine.handle(bots.SERVER_GAME_UPDATE, struct.pack(">I", 1))
    machine.tick(time.monotonic())
    assert len(sent) == 6
    for data in sent:
        name = names[data[-1]]
        assert len(data) - 4 == sizes[name], name


# ---------- the wire format (tools/network_format.py)


def test_network_format_is_the_recorded_one():
    """what the machines send each other is what port/linux/network_format.txt
    records for HALO_PORT_NETWORK_VERSION, and NETCODE.md says what that
    version changed"""
    from tools import network_format

    problems = network_format.check()
    assert not problems, "\n".join(problems)


# ---------- the tag validator (port/linux/game/tag_validate.c)

RETAIL_MAPS = Path("assets/maps")
MAP_VALIDATE = Path("build/linux/map_validate")


def _retail_maps():
    maps = sorted(RETAIL_MAPS.glob("*.map"))
    if not maps or not MAP_VALIDATE.is_file():
        pytest.skip("needs the retail maps in assets/maps and ninja linux's build/linux/map_validate")
    return maps


def test_retail_maps_need_no_corrections():
    """every retail map in assets/maps passes the tag validator, each of its
    structure bsps too, with no correction: the schemas (tag_schema_*.c)
    say what the game's own maps hold"""
    maps = _retail_maps()
    result = subprocess.run([str(MAP_VALIDATE), "--strict", *map(str, maps)], capture_output=True, text=True,
                            timeout=1800)
    assert result.returncode == 0, result.stdout[-6000:]


def test_tag_validator_survives_damaged_maps():
    """retail maps with a few of their tags' words changed at random: the
    validator never crashes or hangs, and a map it lets through is clean
    when checked again"""
    maps = _retail_maps()
    chosen = [path for path in maps if path.stem in ("bloodgulch", "a10", "ui")] or maps[:1]
    result = subprocess.run([str(MAP_VALIDATE), "--fuzz", "300", "--seed", "1", *map(str, chosen)],
                            capture_output=True, text=True, timeout=1800)
    assert result.returncode == 0, (result.stdout + result.stderr)[-6000:]


# ---------- Custom Edition maps (port/linux/game/cache_file_formats.c, custom_edition_cache.c)

def _custom_edition_maps():
    """the Custom Edition maps (HALO_CUSTOM_EDITION_MAPS, or the gitignored
    assets/custom_edition), with the bitmaps.map, sounds.map and loc.map
    they need beside them"""
    configured = os.environ.get("HALO_CUSTOM_EDITION_MAPS")
    folder = Path(configured) if configured else Path("assets/custom_edition")
    maps = [path for path in sorted(folder.glob("*.map"))
            if path.stem.lower() not in ("bitmaps", "sounds", "loc")]
    if not maps or not (folder / "bitmaps.map").is_file() or not MAP_VALIDATE.is_file():
        pytest.skip("needs Custom Edition maps (HALO_CUSTOM_EDITION_MAPS or assets/custom_edition) and "
                    "ninja linux's build/linux/map_validate")
    return maps


def test_custom_edition_maps_are_loaded_and_checked():
    """every Custom Edition map is loaded, converted and passed by the tag
    validator, each of its structure bsps too: none is refused (a community
    map's own mistakes are corrected and logged)"""
    maps = _custom_edition_maps()
    result = subprocess.run([str(MAP_VALIDATE), *map(str, maps)], capture_output=True, text=True, timeout=3600)
    assert result.returncode == 0, result.stdout[-6000:]
    assert "refused" not in result.stdout, result.stdout[-6000:]


def test_custom_edition_loader_and_validator_survive_damaged_maps():
    """Custom Edition maps with a few of their file's words changed at random:
    the loader, the conversion and the validator never crash or hang, and a
    map they let through is clean when checked again"""
    maps = _custom_edition_maps()
    chosen = [path for path in maps if path.stem.lower() in ("bloodgulch", "hugeass", "ui")] or maps[:2]
    result = subprocess.run([str(MAP_VALIDATE), "--fuzz", "100", "--seed", "1", *map(str, chosen)],
                            capture_output=True, text=True, timeout=3600)
    assert result.returncode == 0, (result.stdout + result.stderr)[-6000:]


# ---------- the long-nose Master Chief (port/linux/game/long_nose_chief_geometry.c)

def test_long_nose_chief_geometry(tmp_path):
    """the long nose's and the smile's pieces, made against a made-up helmet:
    counts, bounds, normals, winding, texture coordinates and strips
    (tools/long_nose_chief_check.c). The module also builds alone as the
    game builds its own (gnu89), warnings as errors."""
    clang = shutil.which("clang")
    if not clang:
        pytest.skip("needs clang")
    module = "port/linux/game/long_nose_chief_geometry.c"
    strict = ["-Wall", "-Wextra", "-Werror"]
    built = subprocess.run([clang, "-std=gnu89", *strict, "-c", "-o", str(tmp_path / "geometry.o"), module],
                           capture_output=True, text=True)
    assert built.returncode == 0, built.stderr[-4000:]
    program = tmp_path / "long_nose_chief_check"
    built = subprocess.run([clang, "-std=c99", *strict, "-fsanitize=undefined", "-fsanitize-trap=undefined", "-O1",
                            "-o", str(program), "tools/long_nose_chief_check.c", module, "-lm"],
                           capture_output=True, text=True)
    assert built.returncode == 0, built.stderr[-4000:]
    result = subprocess.run([str(program)], capture_output=True, text=True, timeout=60)
    assert result.returncode == 0, result.stdout[-6000:]
    assert "PASS" in result.stdout


SKATE_SOUND_LOCAL = re.compile(r"halo-skate: sound: (\w+) \(([\d.]+), ([\d.]+) m/s\) (.*)")
SKATE_SOUND_REMOTE = re.compile(r"halo-skate: sound \(remote player (\d+)\): (\w+) \(([\d.]+), ([\d.]+) m/s\) (.*)")


def _test_address(last):
    """a loopback address of the dedicated server tests, 127.0.0.<last>, or
    of HALO_TEST_DEDICATED_NET's network ("127.0.5" for 127.0.5.<last>), so
    that a second run of them can go beside the first on one machine"""
    return f"{os.environ.get('HALO_TEST_DEDICATED_NET', '127.0.0')}.{last}"


def _game_env(env):
    """a game's environment, with tools/stall_backtrace.c preloaded when
    HALO_TEST_STALL_BACKTRACE names it built (-m32, as the game is): a game
    that hangs then tells where (_stall_backtraces)"""
    preload = os.environ.get("HALO_TEST_STALL_BACKTRACE")
    return dict(env, LD_PRELOAD=preload) if preload and Path(preload).is_file() else env


def _elf_function_names(executable, addresses):
    """each of addresses (hex strings) in the 32-bit ELF executable as the
    function it falls in, "name+0x12", from the file's own symbol table (a
    runner without addr2line still says where a hung game is); none for an
    address outside its functions, or a file that is not one"""
    import bisect
    import struct
    try:
        data = Path(executable).read_bytes()
    except OSError:
        return {}
    if data[:5] != b"\x7fELF\x01":
        return {}
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]
    functions = []
    for section in sections:
        # (SHT_SYMTAB, its strings in the section it links to)
        if section[1] != 2:
            continue
        strings = sections[section[6]][4]
        for offset in range(section[4], section[4] + section[5], 16):
            name, value, size, info = struct.unpack_from("<IIIB", data, offset)
            if info & 0xF == 2 and value:
                end = data.index(b"\0", strings + name)
                functions.append((value, size, data[strings + name:end].decode(errors="replace")))
    functions.sort()
    starts = [f[0] for f in functions]
    names = {}
    for address in addresses:
        value = int(address, 16)
        index = bisect.bisect_right(starts, value) - 1
        if index >= 0 and value < functions[index][0] + max(functions[index][1], 1):
            names[address] = f"{functions[index][2]}+{value - functions[index][0]:#x}"
    return names


def _stall_backtraces(name, process, executable, log_path):
    """every thread of a game that is still running where it should not be
    (it hung, or the test fails with it) writes its backtrace to its log
    (tools/stall_backtrace.c, preloaded by _game_env; nothing without it),
    printed for the test's output, through addr2line where it has one, else by the
    executable's own symbol table (_elf_function_names)"""
    if "LD_PRELOAD" not in _game_env({}) or process.poll() is not None or os.uname().machine != "x86_64":
        return
    import ctypes
    import signal
    syscall = ctypes.CDLL(None).syscall
    try:
        threads = sorted(int(tid) for tid in os.listdir(f"/proc/{process.pid}/task"))
    except OSError:
        return
    states = {}
    for tid in threads:
        try:
            states[tid] = Path(f"/proc/{process.pid}/task/{tid}/stat").read_text().rsplit(")", 1)[1].split()[0]
        except OSError:
            pass
        # (tgkill, system call 234 on x86-64)
        syscall(234, process.pid, tid, signal.SIGUSR2)
        time.sleep(0.2)
    time.sleep(1)
    lines = [line.split(": ", 1)[1] for line in Path(log_path).read_text(errors="replace").splitlines()
             if line.startswith("stall-backtrace: ")]
    addresses = [line for line in lines if line.startswith("0x")]
    names = {}
    if addresses and shutil.which("addr2line"):
        found = subprocess.run(["addr2line", "-f", "-C", "-p", "-e", executable, *addresses], capture_output=True,
                               text=True).stdout.splitlines()
        names = dict(zip(addresses, found))
    elif addresses:
        names = _elf_function_names(executable, addresses)
    print(f"== the {name} game (pid {process.pid}), still running: its threads' states {states}, backtraces:")
    for line in lines:
        print(f"   {line} {names.get(line, '')}" if line.startswith("0x") else f"  {line}")


def _assert_games_ended(killed, logs, seconds, folder):
    """that no game of a test had to be stopped at its deadline: each exits
    on its own after its seconds (debug.exit_after). One that did not hung
    (a fatal error leaves a game on its error screen, which a game without a
    window shows nobody: its log says so) or was too slow to keep up: said
    with the last tick its log has, and the end of its debug.txt (folder /
    the game's name / data or saves)"""
    for name in killed:
        ticks = re.findall(r"network test: tick (\d+)", logs[name])
        last = f"its log stops at tick {ticks[-1]} ({int(ticks[-1]) // 30} s into the game)" if ticks else \
            "its log has no tick of a game"
        debug = [path for path in (folder / name / "data" / "debug.txt", folder / name / "saves" / "debug.txt")
                 if path.is_file()]
        raise AssertionError(f"the {name} game did not exit after its {seconds} s and was stopped (it hung, or "
                             f"could not keep up); {last}:\n{logs[name][-4000:]}\nits debug.txt:\n"
                             f"{debug[0].read_text(errors='replace')[-2000:] if debug else ''}")


def _assert_remote_skate_sounds(logs, hears):
    """each machine in hears (name: the machine whose skater it hears) played
    that machine's skater's one-shots as its own skater's machine told of
    them (Skate 3 mode's sounds in a network game: skate_network.c,
    sound_remote.rs): told of in its log as another player's, that machine's
    player and never its own, each with a sample (but those the data has none
    for, silent on the skater's own machine too, as grind_end), nearly all of them (a tenth
    of the datagrams lost loses none: each one-shot goes for half a second; a
    few are missed while the skater is dead here, or as the game ends), and
    none twice"""
    own = {name: set(re.findall(r"skate network: player (\d+) skates here", log)) for name, log in logs.items()}
    for name, other in hears.items():
        made_lines = SKATE_SOUND_LOCAL.findall(logs[other])
        made = [m[0] for m in made_lines]
        silent_made = {m[0] for m in made_lines if m[3].startswith("silent")}
        heard = SKATE_SOUND_REMOTE.findall(logs[name])
        players = {h[0] for h in heard}
        print(f"the {name} machine heard {len(heard)} one-shots of player(s) {sorted(players)}, of the {other} "
              f"machine's {len(made)} (its players {sorted(own[other])}): "
              f"{sorted(set(h[1] for h in heard))}, e.g. {heard[:3]}")
        # (first that there was something to hear: the other machine's player
        # skated and their skater made one-shots)
        assert own[other], f"the {other} machine's player never skated:\n{logs[other][-4000:]}"
        assert made, f"the {other} machine's skater made no one-shot to hear:\n{logs[other][-4000:]}"
        assert heard, f"the {name} machine heard none of the {other} machine's skater's {len(made)} one-shots"
        assert players <= own[other], f"the {name} machine heard players {players}, not the {other}'s {own[other]}"
        assert not players & own[name], f"the {name} machine heard its own skater as another's: {players}"
        assert all(h[4] and (not h[4].startswith("silent") or h[1] in silent_made) for h in heard), \
            f"a one-shot had no sample here but one on its skater's machine: {heard}, silent there: {silent_made}"
        assert {h[1] for h in heard} <= set(made), f"sounds heard that were never made: {heard}, {set(made)}"
        assert len(heard) >= 0.75 * len(made), f"the {name} machine heard {len(heard)} of {len(made)} one-shots"
        assert len(heard) <= len(made), f"the {name} machine heard {len(heard)} one-shots of {len(made)} made"


SKATE_RUN_OVER_VERDICT = re.compile(r"player (\d+)'s skater (bails|rides on); player (\d+) on foot run over, "
                                    r"([\d.]+) of a vehicle's hit")
SKATE_RUN_OVER_DEALT = re.compile(r"skate verdict (\d+): player (\d+)'s skater ran over a player here, ([\d.]+) of "
                                  r"Halo's vehicle collision damage: shield ([\d.-]+) to ([\d.-]+), body ([\d.-]+) "
                                  r"to ([\d.-]+)"
                                  r"(, killed)?")
SKATE_RUN_OVER_DEATH = re.compile(r"skate verdict (\d+): the person run over died here, killed by player (-?\d+)"
                                  r"(, friendly fire)? \(the skater, player (\d+)\): player -?\d+ has (\d+) kills, "
                                  r"player (\d+) (\d+) deaths")


def _assert_run_over(decided, host, skater_log, skater, logs):
    """a skater (player skater, whose machine's log is skater_log) ran a
    person on foot over as a vehicle does (skate_authority.c): the host
    (host, its verdicts decided) decided a hit on them at speed a run-over,
    and dealt them Halo's whole vehicle collision damage, which killed them;
    on the skater's machine the engine went on through them. Every run-over
    (of any skater: another, on foot a moment, may be run over too) has its
    skater riding on, never bailing for it, its damage theirs, and the kill
    theirs on every machine of logs (each saw one at least). No verdict on two
    skaters is a run-over."""
    runs = {n: m.groups() for n, text in decided.items() if (m := SKATE_RUN_OVER_VERDICT.search(text))}
    dealt = SKATE_RUN_OVER_DEALT.findall(host)
    deaths = {name: SKATE_RUN_OVER_DEATH.findall(log) for name, log in logs.items()}
    gone_through = re.findall(r"halo-skate: ran over a person at ([\d.]+) m/s", skater_log)
    print(f"run-overs decided {runs}; dealt {dealt}; deaths {deaths}; gone through on the skater's machine at "
          f"{gone_through} m/s")
    assert runs, f"no hit on a person on foot was a run-over: {decided}"
    assert any(r[0] == skater for r in runs.values()), f"player {skater}'s skater ran nobody over: {runs}"
    assert all(r[1] == "rides on" for r in runs.values()), f"a skater bailed running over: {runs}"
    assert not any(re.search(r"; player \d+'s skater", text) and "run over" in text for text in decided.values()), \
        f"a skater was run over: {decided}"
    assert gone_through, f"the skater's engine never went through the person:\n{skater_log[-4000:]}"
    killed = [d for d in dealt if d[7] and float(d[2]) == 1.0 and d[1] == skater]
    # (every other time the watcher steps in the scripted skater is pushed on
    # to 8.5 m/s first, skate.c's skate_test_run_over_boost, so one at least
    # is past Halo's lethal 6.1 m/s whatever speed the ride had)
    boosted = re.findall(r"skate test: a person on foot in the way, the skater pushed on from ([\d.]+) m/s", skater_log)
    speeds = {n: float(re.search(r"; at ([\d.]+) m/s", decided[n]).group(1)) for n in runs}
    assert killed, f"no run-over at speed killed with the whole of Halo's vehicle collision damage: {dealt}; the " \
        f"run-overs' speeds {speeds} m/s, the skater pushed on from {boosted} m/s"
    assert all(int(d[0]) in runs and d[1] == runs[int(d[0])][0] for d in dealt), \
        f"damage dealt not of the skater's run-overs: {dealt}"
    for name, seen in deaths.items():
        assert seen, f"no death of a person run over seen on the {name} machine:\n{logs[name][-4000:]}"
        assert all(int(d[0]) in runs and d[1] == d[3] == runs[int(d[0])][0] and not d[2] for d in seen), \
            f"a person run over was not killed by their skater on the {name} machine: {seen}"
    kills = [int(d[4]) for d in SKATE_RUN_OVER_DEATH.findall(host) if d[1] == skater]
    assert kills == sorted(kills) and kills[-1] >= len(kills), f"the skater's kills did not count up: {kills}"


def test_skate_multiplayer(tmp_path):
    """Skate 3 mode in a network game, two real games on this machine: a host
    and a client on Blood Gulch, the client getting on a board and riding a
    scripted pad (debug.network_test_skate), the host watching
    (debug.network_test_skate_watch) and stepping into their way every five
    seconds (debug.network_test_skate_block), on foot at first, then on a board
    of its own (debug.network_test_skate_duel). The host must pose the
    client's skater from its state and draw their board, and take every place
    the client gives (but a stray few), the client's state must rebuild its
    own pose within a hair, the skater must hit the host's player (an obstacle
    on the client: skate.c) rather than pass by: on foot, fast, run over as by a
    vehicle (on through them, the host's damage killing them, the kill the
    skater's), and skating a bump or a bail, and
    the host's skater the client's (each machine's skater solid to the other's);
    and each loads Blood Gulch's scenery into the level's collision. With a
    round trip of 150 ms and a tenth of the datagrams lost. It needs a Linux build
    with --skate (HALO_TEST_GAME), the converted Skate 3 data
    (HALO_SKATE_TEST_ASSETS) and the Halo maps (HALO_SKATE_TEST_HALO_MAPS),
    and skips without them; about two and a half minutes."""
    game = os.environ.get("HALO_TEST_GAME")
    assets = os.environ.get("HALO_SKATE_TEST_ASSETS")
    maps = os.environ.get("HALO_SKATE_TEST_HALO_MAPS")
    if not (game and assets and maps and Path(game).is_file() and Path(maps, "bloodgulch.map").is_file()):
        pytest.skip("needs HALO_TEST_GAME, HALO_SKATE_TEST_ASSETS and HALO_SKATE_TEST_HALO_MAPS")
    seconds = int(os.environ.get("HALO_TEST_SKATE_SECONDS", "110"))
    common = {"HALO_FULLSCREEN": "0", "HALO_NO_AUDIO": "1", "HALO_EXIT_AFTER": str(seconds),
              "HALO_UPDATE_ANSWER": "never", "HALO_NULL_RENDERER": "1", "HALO_HIDDEN_WINDOW": "1",
              "HALO_SKATE_ASSETS": assets, "HALO_NETWORK_LATENCY": "75", "HALO_NETWORK_LOSS": "10",
              # (the same datagrams lost, and the game's random numbers, each run)
              "HALO_GAME_SEED": "1101",
              # (no STUN: the two play over loopback, and a CI runner may not
              # reach the STUN servers)
              "HALO_NET_STUN": ""}
    # (HALO_TEST_SKATE_NET: the host's and the client's loopback addresses,
    # "127.0.0.210,127.0.0.211" by default; other pairs let runs go side by
    # side, as tools/skate_mp_loop.sh runs them)
    host_address, join_address = os.environ.get("HALO_TEST_SKATE_NET", "127.0.0.210,127.0.0.211").split(",")
    machines = {"host": {"HALO_NET_ADDRESS": host_address, "HALO_NET_BROADCAST": join_address,
                         "HALO_NETWORK_TEST": "host:bloodgulch", "HALO_NETWORK_TEST_START": "12",
                         "HALO_NETWORK_TEST_SKATE_WATCH": "1", "HALO_NETWORK_TEST_SKATE_BLOCK": "1",
                         "HALO_NETWORK_TEST_SKATE_DUEL": "56",
                         "HALO_NETWORK_TEST_SKATE_SHOOT": "1", "HALO_NETWORK_TEST_SKATE_TELEPORT": "66"},
                "join": {"HALO_NET_ADDRESS": join_address, "HALO_NET_BROADCAST": host_address,
                         "HALO_NETWORK_TEST": "join", "HALO_NETWORK_TEST_SKATE": "15",
                         "HALO_NETWORK_TEST_SKATE_SCENERY": "30"}}
    games = {}
    killed = []
    # (each exits after its seconds, debug.exit_after, and is stopped at a
    # deadline too, whatever happens, so that no game outlives the test, on a
    # CI runner least of all)
    try:
        for name, settings in machines.items():
            folder = tmp_path / name
            (folder / "data").mkdir(parents=True)
            (folder / "saves").mkdir()
            (folder / "data" / "maps").symlink_to(maps)
            env = dict(os.environ, **common, **settings, HALO_DATA_ROOT=str(folder / "data"),
                       HALO_SAVE_ROOT=str(folder / "saves"))
            with open(folder / "run.log", "w") as log:
                games[name] = subprocess.Popen([game], cwd=folder, env=_game_env(env), stdout=log,
                                               stderr=subprocess.STDOUT)
            if name == "host":
                time.sleep(4)
        deadline = time.monotonic() + seconds + 30
        for process in games.values():
            try:
                process.wait(timeout=max(1.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                pass
    finally:
        for name, process in games.items():
            if process.poll() is None:
                _stall_backtraces(name, process, game, tmp_path / name / "run.log")
                process.kill()
                killed.append(name)
            process.wait()
    host = (tmp_path / "host" / "run.log").read_text(errors="replace")
    join = (tmp_path / "join" / "run.log").read_text(errors="replace")
    # (a game that did not end on its own after its seconds hung, or nearly:
    # said first, as what the rest of the test misses follows from it)
    _assert_games_ended(killed, {"host": host, "join": join}, seconds, tmp_path)
    remote = re.findall(r"skate network: player \d+ skates on another machine at .*?: (posed|not posed,) (\d+) nodes, "
                        r"(the board drawn|no board), .*?host took (\d+) of its places, refused (\d+)", host)
    local = [float(m) for m in re.findall(r"skates here at .*? rebuilt within ([\d.]+) world units", join)]
    assert "skate test: on the board" in join, join[-4000:]
    assert len(local) >= 20, f"the client skated {len(local)} seconds:\n{join[-4000:]}"
    assert max(local) < 0.01, f"the client's state rebuilt its pose {max(local)} world units off"
    assert len(remote) >= 20, f"the host saw the skater {len(remote)} seconds:\n{host[-4000:]}"
    posed = [r for r in remote if r[0] == "posed" and r[2] == "the board drawn"]
    assert len(posed) >= 0.9 * len(remote), f"posed with the board {len(posed)} of {len(remote)} seconds"
    taken, refused = int(remote[-1][3]), int(remote[-1][4])
    assert refused <= max(3, taken // 50), f"the host refused {refused} of the skater's places, took {taken}"
    # each machine hears the other's skater (skate_network.c's sounds,
    # messages 94 and 95; halo-skate's sound_remote.rs): the host the
    # client's from the start, the client the host's once it skates (the
    # duel), each one-shot of theirs once, never its own skater as another's
    _assert_remote_skate_sounds({"host": host, "join": join}, {"host": "join", "join": "host"})
    # the watcher steps into the skater's way every five seconds
    # (debug.network_test_skate_block): on the client it is another machine's
    # biped, an obstacle (skate.c), which the skater must hit - bump, or run
    # them over and go on through, as a vehicle does - rather than pass by
    blocks = re.findall(r"the watcher steps into the skater's way at", host)
    near = [int(n) for n in re.findall(r"skate test: (\d+) obstacles near", join)]
    hits = re.findall(r"halo-skate: hit a biped at ([\d.]+) m/s", join)
    outcomes = re.findall(r"halo-skate: hit a biped at [\d.]+ m/s [^\n]*: (bailed|rode on)", join)
    run_overs = re.findall(r"halo-skate: ran over a person at ([\d.]+) m/s", join)
    print(f"{len(blocks)} blocks; obstacles near in {len(near)} logged seconds; {len(hits)} hits at {hits} m/s: "
          f"{outcomes}; ran the watcher over at {run_overs} m/s")
    assert len(blocks) >= 3, f"the watcher stepped in {len(blocks)} times:\n{host[-4000:]}"
    assert near, f"the client never had the watcher as an obstacle:\n{join[-4000:]}"
    assert hits or run_overs, f"the skater never hit the watcher:\n{join[-4000:]}"
    # later the host's player skates too (debug.network_test_skate_duel) and
    # is put on its board in the skater's way: each machine has the other's
    # skater as their body and board (skate.c's skate_skater_obstacles), and
    # the skater riding in hits the host's skater on the client, and on the
    # host the client's skater hits the host's
    duels = re.findall(r"the watcher steps into the skater's way on a board", host)
    skater_parts = [int(n) for n in re.findall(r"obstacles near \(\d+ bipeds, (\d+) of skaters", join)]
    join_hits = re.findall(r"halo-skate: hit a skater at [\d.]+ m/s [^\n]*: (bailed|rode on)", join)
    host_hits = re.findall(r"halo-skate: hit a skater at [\d.]+ m/s [^\n]*: (bailed|rode on)", host)
    print(f"{len(duels)} duels; a skater near in {sum(1 for n in skater_parts if n)} logged seconds; "
          f"the client's skater hit the host's {join_hits}, the host's the client's {host_hits}")
    assert "skate test: on the board, to stand in the skater's way" in host, host[-4000:]
    assert len(duels) >= 2, f"the host's skater stepped in {len(duels)} times:\n{host[-4000:]}"
    assert any(skater_parts), f"the client never had the host's skater as an obstacle:\n{join[-4000:]}"
    assert join_hits, f"the client's skater never hit the host's:\n{join[-4000:]}"
    assert host_hits, f"the host's skater was never hit by the client's:\n{host[-4000:]}"
    # every one of those hits is the host's to decide (skate_authority.c):
    # each verdict once, the same on both machines (who bails, the push on
    # the watcher on foot), each bail applied on the skater's own machine and
    # seen on the other, and the watcher pushed on both
    decided = dict((int(n), text) for n, text in re.findall(r"skate verdict (\d+) decided: (.*)", host))
    heard = dict((int(n), text) for n, text in re.findall(r"skate verdict (\d+) heard: (.*)", join))
    bails = {n: (text.count("bails"), heard.get(n, "").count("bails")) for n, text in decided.items()}
    print(f"verdicts decided {decided}; heard {heard}; bails on the host and the client {bails}")
    assert any("on foot" in text for text in decided.values()), f"no hit on the watcher on foot decided: {decided}"
    assert any(re.search(r"; player \d+'s skater", text) for text in decided.values()), \
        f"no skater on skater decided: {decided}"
    missing = [n for n in decided if n not in heard and n != max(decided)]
    assert not missing, f"the client never heard verdicts {missing}: {decided}, {heard}"
    assert all(heard[n] == decided.get(n) for n in heard), f"the machines' verdicts differ: {decided}, {heard}"
    assert all(h == c for n, (h, c) in bails.items() if n in heard), f"bails differ: {bails}"
    # (each machine's own player, whichever index the game gave it)
    players = {name: set(re.findall(r"skate network: player (\d+) skates here", log)) for name, log in
               (("host", host), ("join", join))}
    # the client's skater riding fast into the watcher on foot runs them over,
    # as a vehicle would: on through them, killed by the host's damage, the
    # kill the skater's; two skaters still bump (the duel's verdicts above)
    (skater,) = players["join"]
    _assert_run_over(decided, host, join, skater, {"host": host, "join": join})
    for n, text in decided.items():
        for player, own in [(p, join) for p in players["join"]] + [(p, host) for p in players["host"]]:
            if f"player {player}'s skater bails" in text and n in heard:
                assert f"skate verdict {n}: this machine's skater bails" in own, f"verdict {n} not applied: {text}"
    seen = len(re.findall(r"skate network: player \d+ bails on another machine", host + join))
    assert sum(h for h, _ in bails.values()) == 0 or seen, f"no bail decided was seen on the other machine: {bails}"
    went = {m: [(int(n), float(d)) for n, d in re.findall(r"skate verdict (\d+): the person pushed went ([\d.-]+) "
                                                         r"world units", log)] for m, log in (("host", host), ("join", join))}
    print(f"bails seen on the other machine {seen}; pushes measured {went}")
    both = [n for n, d in went["host"] if d > 0.05 and any(m == n and e > 0.05 for m, e in went["join"])]
    assert both, f"the watcher was not seen pushed on both machines: {went}"
    # Blood Gulch's scenery goes to the engine with the level's collision
    # (skate.c's skate_add_scenery), as solid as the level and its edges rails
    scenery = re.findall(r"skate: the level's collision: (\d+) triangles, (\d+) of them (\d+) pieces of scenery",
                         join)
    print(f"the level's collision: {scenery}")
    assert scenery and int(scenery[-1][2]) > 0, f"no scenery in the level's collision:\n{join[:4000]}"
    # the client deletes the piece of scenery nearest its player, and makes
    # it again four seconds on (debug.network_test_skate_scenery): each time
    # the level's collision is built again (skate.c's skate_watch_scenery),
    # with a piece fewer, then as before, and the engine swaps it in
    assert "skate test: deleted a piece of scenery" in join, join[-4000:]
    assert "skate test: made the piece of scenery again" in join, join[-4000:]
    again = [int(n) for n in re.findall(r"skate: the level's collision again, its scenery changed: \d+ triangles, "
                                        r"\d+ of them (\d+) pieces of scenery", join)]
    rebuilt = re.findall(r"halo-skate: map rebuilt, (\d+) triangles", join)
    print(f"the level built again with {again} pieces of scenery; {len(rebuilt)} rebuilds swapped in")
    pieces = int(scenery[-1][2])
    assert again[:2] == [pieces - 1, pieces], f"rebuilt with {again} pieces, not {pieces - 1} then {pieces}"
    assert len(rebuilt) >= 2, f"the rebuilds were not swapped in:\n{join[-4000:]}"
    # the host's watcher shoots skaters with real projectiles
    # (debug.network_test_skate_shoot), the client's where the host has them,
    # and its own once it skates: a burst hits the posed body and damages it,
    # a grenade thrown at their chest bounces off them without pushing them,
    # and its blast pushes them away from it as it pushes a player on foot
    # (skate.c gives Halo's push to the skate engine, on the skater's own
    # machine: the client's replays the host's damage to its skater)
    bullets = re.findall(r"skate test: a burst at a skater \((this|another) machine's\): shield ([\d.-]+) to "
                         r"([\d.-]+), body ([\d.-]+) to ([\d.-]+)", host)
    damaged = {who for who, s0, s1, b0, b1 in bullets if float(s1) < float(s0) or float(b1) < float(b0)}
    bounced = re.findall(r"skate test: the grenade bounced off the skater \((this|another) machine's\)", host)
    applied = (r"skate: a push of ([\d.]+) m/s on the skater applied at tick (\d+) \((on|off) the board\), along "
               r"\(([\d.-]+) ([\d.-]+) [\d.-]+\)")
    pushes = [(float(speed), int(tick), board, (float(x), float(y))) for speed, tick, board, x, y in
              re.findall(applied, host)]
    pushes_join = [(float(speed), int(tick), board) for speed, tick, board, _, _ in re.findall(applied, join)]
    undone = re.findall(r"skate: a push of ([\d.]+) m/s on the skater undone", host)
    print(f"bullets {bullets}; damaged {damaged}; grenades bounced off {bounced}; pushes applied {pushes}, on the "
          f"client {pushes_join}; undone {undone}")
    assert damaged == {"this", "another"}, f"a bullet did not damage a skater on each machine: {bullets}"
    assert bounced, f"no grenade bounced off a skater:\n{host[-4000:]}"
    # (the grenades at the host's own skater, thrown alone: no push till they
    # go off, bounce or not; one as each goes off on its fuse hurting them,
    # and they go away from it)
    events = re.findall(r"skate test: (?:shooting a skater \(this machine's\) [\d.]+ world units away, a burst \(none\) "
                        r"and a grenade at tick (\d+)|the grenade went off at tick (\d+) \((after|without) bouncing off "
                        r"them\), [\d.]+ world units from the skater \(this machine's\): shield ([\d.-]+) to ([\d.-]+), "
                        r"body ([\d.-]+) to ([\d.-]+)|after the grenade the skater \(this machine's\) .*?body ([\d.-]+); "
                        r"([\d.-]+) world units further .*?along \(([\d.-]+) ([\d.-]+)\))", host)
    throws = []
    for thrown, off, bounce, s0, s1, b0, b1, body, further, away_x, away_y in events:
        if thrown:
            throws.append({"thrown": int(thrown)})
        elif off and throws:
            throws[-1].update(off=int(off), bounce=bounce, hurt=(float(s1) < float(s0) or float(b1) < float(b0)) and
                              float(b1) > 0)
        elif further and throws:
            throws[-1].update(further=float(further), alive=float(body) > 0, away=(float(away_x), float(away_y)))
    for throw in throws:
        throw["early"] = [p for p in pushes if throw["thrown"] <= p[1] < throw.get("off", throw["thrown"])]
        throw["blast"] = [p for p in pushes if "off" in throw and throw["off"] <= p[1] <= throw["off"] + 2]
    # (a grenade that went off on its fuse, a second and a half on, hurting
    # the skater without killing them, who were alive still a third of a
    # second on, when the push was measured)
    blown = [t for t in throws if "off" in t and 40 <= t["off"] - t["thrown"] <= 60 and t["hurt"] and t.get("alive")]
    print(f"grenades at the host's own skater: {throws}")
    assert throws, f"no grenade was thrown at the host's own skater:\n{host[-4000:]}"
    assert not any(t["early"] for t in throws), f"a grenade pushed the skater before it went off: {throws}"
    assert blown, f"no grenade went off at the host's own skater, hurting them: {throws}"
    # (each pushed them once, away from the blast: level, within 45 degrees
    # of the way from the grenade to them; and one, at least, moved them
    # measurably further from it than their course. Not every one: a skater
    # left on Blood Gulch's teleporter exit by the teleport test can stand
    # unmoved by a blast pushed into the engine, and the host throws again
    # till one moves them more than SKATE_PUSH_AWAY, 0.08, skate_network.c.
    # The log rounds to the hundredth, so a push the game counted (say 0.083)
    # reads 0.08: at least that, then, or it would fail one it stopped on)
    for t in blown:
        assert len(t["blast"]) == 1, f"the blast did not push the host's skater once: {t}"
        (x, y), (ax, ay) = t["blast"][0][3], t["away"]
        assert x * ax + y * ay > 0.7 * math.hypot(x, y), f"the blast did not push the skater away from it: {t}"
    assert any(t["further"] >= 0.08 for t in blown), f"no blast moved the skater away from it: {blown}"
    assert not undone, f"a push was undone: {undone}"
    # (the client pushes its own skater for a blast the host's grenade gave
    # them, replaying the host's damage, once: a push a little after each
    # grenade that went off on its fuse at its skater on the host and hurt
    # them without killing them (a killed skater's body flies as anyone's))
    shots = [int(t) for t in re.findall(r"skate test: shooting a skater \(another machine's\) [\d.]+ world units away, "
                                        r"a burst(?: \([^)]*\))? and a grenade at tick (\d+)", host)]
    blasts = re.findall(r"skate test: the grenade went off at tick (\d+) \([a-z]+ bouncing off them\), [\d.]+ world "
                        r"units from the skater \(another machine's\): shield ([\d.-]+) to ([\d.-]+), body ([\d.-]+) "
                        r"to ([\d.-]+)", host)
    hurt = [int(t) for t, s0, s1, b0, b1 in blasts if (float(s1) < float(s0) or float(b1) < float(b0)) and
            float(b1) > 0 and any(40 <= int(t) - shot <= 60 for shot in shots)]
    # (the client's game time is not the host's to the tick: a client ahead
    # of the host by its clock pushes a tick or two before the host's tick
    # of the blast, as seen in a run: 644 for 645)
    replayed = [p for p in pushes_join if any(-15 <= p[1] - t <= 30 for t in hurt)]
    print(f"grenades at the client's skater {blasts}, hurting them at {hurt}; the client's pushes {pushes_join}")
    assert len(replayed) == len(hurt), f"the client's skater was not pushed once by each blast: {replayed}, {hurt}"
    # and the host puts its own skater on a teleporter's entrance
    # (debug.network_test_skate_teleport): Halo's teleporter takes them to
    # its exit, turned, and the skate engine's skater follows, keeping its
    # speed
    teleports = re.findall(r"skate: teleported ([\d.]+) world units to \(([\d.-]+) ([\d.-]+) [\d.-]+\), turned "
                           r"(-?\d+) degrees, at ([\d.]+) m/s", host)
    print(f"teleports {teleports}")
    assert "skate test: this machine's skater put on a teleporter's entrance" in host, host[-4000:]
    exits = [t for t in teleports if abs(float(t[1]) - 82.83) < 1.5 and abs(float(t[2]) + 114.53) < 1.5]
    assert exits, f"the skater did not follow the teleporter to its exit: {teleports}"


SKATE_HITBOX_LINE = re.compile(
    r"skate hitbox: (this|another) machine's skater, (before|after) objects, (\S+): shapes off by ([\d.]+) world units "
    r"at most .*?the bounding sphere ([\d.]+) off, the last drawn ([\d.-]+) off; lines at the head (\d+) \(\d+ on it "
    r"drawn\): (\d+) alike, (\d+) elsewhere, (\d+) missed; torso (\d+) \(\d+\): (\d+), (\d+), (\d+); arms (\d+) "
    r"\(\d+\): (\d+), (\d+), (\d+); legs (\d+) \(\d+\): (\d+), (\d+), (\d+); past the body (\d+): (\d+) hit drawn, "
    r"(\d+) hit; the drawn shapes ([\d.]+) past the bounding sphere")


def test_skate_hitbox(tmp_path):
    """A skater is hit where their body is drawn (port/linux/game/skate_hitbox.c):
    two real games on Blood Gulch, the client riding the scripted pad
    (debug.network_test_skate: pushes, carves, ollies and, with
    debug.skate_hitbox_test, flip tricks), the host watching, and both
    checking every skater's hit shapes (debug.skate_hitbox_test) before and
    after objects_update, as projectiles meet them: the client its own
    skater, the host the client's, posed from their state. Each node's
    collision shapes must be where the drawn pose puts them, and the bounding
    sphere too; lines a bullet takes at each part of the drawn body (head,
    torso, arms, legs; from the side and the front) must hit the node they hit
    on the drawn body, and lines just past the body (over the head, either
    side) nothing of the skater. It needs what test_skate_multiplayer
    needs, and skips without it; about a minute and a half."""
    game = os.environ.get("HALO_TEST_GAME")
    assets = os.environ.get("HALO_SKATE_TEST_ASSETS")
    maps = os.environ.get("HALO_SKATE_TEST_HALO_MAPS")
    if not (game and assets and maps and Path(game).is_file() and Path(maps, "bloodgulch.map").is_file()):
        pytest.skip("needs HALO_TEST_GAME, HALO_SKATE_TEST_ASSETS and HALO_SKATE_TEST_HALO_MAPS")
    seconds = 70
    common = {"HALO_FULLSCREEN": "0", "HALO_NO_AUDIO": "1", "HALO_EXIT_AFTER": str(seconds),
              "HALO_UPDATE_ANSWER": "never", "HALO_NULL_RENDERER": "1", "HALO_HIDDEN_WINDOW": "1",
              "HALO_SKATE_ASSETS": assets, "HALO_NET_STUN": "", "HALO_SKATE_HITBOX_TEST": "1"}
    host_address, join_address = os.environ.get("HALO_TEST_SKATE_NET", "127.0.0.210,127.0.0.211").split(",")
    machines = {"host": {"HALO_NET_ADDRESS": host_address, "HALO_NET_BROADCAST": join_address,
                         "HALO_NETWORK_TEST": "host:bloodgulch", "HALO_NETWORK_TEST_START": "12",
                         "HALO_NETWORK_TEST_SKATE_WATCH": "1"},
                "join": {"HALO_NET_ADDRESS": join_address, "HALO_NET_BROADCAST": host_address,
                         "HALO_NETWORK_TEST": "join", "HALO_NETWORK_TEST_SKATE": "15"}}
    games = {}
    killed = []
    try:
        for name, settings in machines.items():
            folder = tmp_path / name
            (folder / "data").mkdir(parents=True)
            (folder / "saves").mkdir()
            (folder / "data" / "maps").symlink_to(maps)
            env = dict(os.environ, **common, **settings, HALO_DATA_ROOT=str(folder / "data"),
                       HALO_SAVE_ROOT=str(folder / "saves"))
            with open(folder / "run.log", "w") as log:
                games[name] = subprocess.Popen([game], cwd=folder, env=_game_env(env), stdout=log,
                                               stderr=subprocess.STDOUT)
            if name == "host":
                time.sleep(4)
        deadline = time.monotonic() + seconds + 30
        for process in games.values():
            try:
                process.wait(timeout=max(1.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                pass
    finally:
        for name, process in games.items():
            if process.poll() is None:
                _stall_backtraces(name, process, game, tmp_path / name / "run.log")
                process.kill()
                killed.append(name)
            process.wait()
    _assert_games_ended(killed, {name: (tmp_path / name / "run.log").read_text(errors="replace") for name in machines},
                        seconds, tmp_path)
    checks = []
    for name in machines:
        log = (tmp_path / name / "run.log").read_text(errors="replace")
        for m in SKATE_HITBOX_LINE.finditer(log):
            who, when, state = m.group(1), m.group(2), m.group(3)
            numbers = [float(x) for x in m.groups()[3:]]
            off, sphere, lag = numbers[0:3]
            parts = [numbers[3 + 4 * i: 7 + 4 * i] for i in range(4)]
            outside, outside_drawn, outside_hit = numbers[19:22]
            unbounded = numbers[22]
            checks.append({"machine": name, "who": who, "when": when, "state": state, "off": off, "sphere": sphere,
                           "lag": lag, "lines": sum(p[0] for p in parts), "alike": sum(p[1] for p in parts),
                           "head": parts[0], "outside": outside, "outside_drawn": outside_drawn,
                           "outside_hit": outside_hit, "unbounded": unbounded})
    own = [c for c in checks if c["machine"] == "join" and c["who"] == "this"]
    others = [c for c in checks if c["machine"] == "host" and c["who"] == "another"]
    states = sorted({c["state"] for c in own})
    lags = sorted(c["lag"] for c in checks if c["lag"] >= 0)
    print(f"{len(own)} checks of the client's own skater (states {states}), {len(others)} of it on the host; "
          f"shapes off at most {max((c['off'] for c in checks), default=None)}, the bounding sphere "
          f"{max((c['sphere'] for c in checks), default=None)}; lines alike {sum(c['alike'] for c in checks)} of "
          f"{sum(c['lines'] for c in checks)}, head {sum(c['head'][1] for c in checks)} of "
          f"{sum(c['head'][0] for c in checks)}; past the body hit {sum(c['outside_hit'] for c in checks)} of "
          f"{sum(c['outside'] for c in checks)}; the last drawn tick off by "
          f"{lags[len(lags) // 2] if lags else None} (median), {lags[-1] if lags else None} at most")
    join_tail = (tmp_path / "join" / "run.log").read_text(errors="replace")[-4000:]
    host_tail = (tmp_path / "host" / "run.log").read_text(errors="replace")[-4000:]
    assert len(own) >= 60, f"the client checked its skater {len(own)} times:\n{join_tail}"
    assert len(others) >= 60, f"the host checked the client's skater {len(others)} times:\n{host_tail}"
    assert any("Air" in state for state in states), f"the skater was never checked in the air: {states}"
    # (where collision has the shapes is where the drawn pose has them, to the
    # log's thousandth of a world unit: before objects_update and after it)
    for check in checks:
        assert check["off"] <= 0.002 and check["sphere"] <= 0.002, f"hit shapes off the drawn body: {check}"
    lines = sum(c["lines"] for c in checks)
    assert lines >= 1000, f"only {lines} lines met the drawn body"
    assert sum(c["alike"] for c in checks) >= 0.99 * lines, f"lines hit elsewhere than on the drawn body: " \
        f"{[c for c in checks if c['alike'] < c['lines']][:5]}"
    assert sum(c["head"][0] for c in checks) >= 50, "too few lines met the drawn head"
    assert sum(c["head"][1] for c in checks) >= 0.99 * sum(c["head"][0] for c in checks), "a headshot went astray"
    # (lines just past the drawn body hit nothing of it, drawn or as hit)
    assert not any(c["outside_drawn"] for c in checks), "a line past the drawn body met its drawn shapes"
    assert not any(c["outside_hit"] for c in checks), \
        f"a line past the drawn body hit the skater: {[c for c in checks if c['outside_hit']][:5]}"
    # (and the bounding sphere collision passes over far objects by holds
    # the drawn body whole)
    assert not any(c["unbounded"] for c in checks), \
        f"the drawn body outside its bounding sphere: {[c for c in checks if c['unbounded']][:5]}"
    # (and what is hit is what was drawn the tick before, but for a tick's
    # travel: a skater at a board's speed goes a tenth of a world unit or so)
    assert lags and lags[len(lags) // 2] < 0.2, f"the hit shapes are far from the last drawn tick: {lags[-20:]}"


def test_dedicated_server(tmp_path):
    """A dedicated server (server.dedicated) and a client on this machine: the
    server starts with no display or SDL driver set and no player of its own,
    its game starts empty, a client (debug.network_test "join") joins it in
    progress and plays (its player in the same place on both machines), the
    server sleeps between its frames, and SIGTERM ends it cleanly - status 0,
    the client told the host has gone. It needs a Linux build
    (HALO_TEST_GAME) and the Halo maps (HALO_SKATE_TEST_HALO_MAPS, as
    test_skate_multiplayer), and skips without them; about a minute."""
    game = os.environ.get("HALO_TEST_GAME")
    maps = os.environ.get("HALO_SKATE_TEST_HALO_MAPS")
    if not (game and maps and Path(game).is_file() and Path(maps, "bloodgulch.map").is_file()):
        pytest.skip("needs HALO_TEST_GAME and HALO_SKATE_TEST_HALO_MAPS")
    host_address, join_address = _test_address(212), _test_address(213)
    machines = {"host": {"HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "5"},
                "join": {"HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1",
                         "HALO_TEST_INPUT": "bot:7"}}
    games = {}
    try:
        for name, settings in machines.items():
            folder = tmp_path / name
            (folder / "data").mkdir(parents=True)
            (folder / "saves").mkdir()
            (folder / "data" / "maps").symlink_to(maps)
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith(("SDL_", "HALO_", "DISPLAY", "WAYLAND"))}
            env.update(settings, HALO_NET_ONLINE="false", HALO_UPDATE_ANSWER="never",
                       HALO_NET_ADDRESS=host_address if name == "host" else join_address,
                       HALO_NET_BROADCAST=join_address if name == "host" else host_address,
                       HALO_DATA_ROOT=str(folder / "data"), HALO_SAVE_ROOT=str(folder / "saves"))
            with open(folder / "run.log", "w") as log:
                games[name] = subprocess.Popen([game], cwd=folder, env=_game_env(env), stdout=log,
                                               stderr=subprocess.STDOUT)
        host_log, join_log = tmp_path / "host" / "run.log", tmp_path / "join" / "run.log"
        # the client's player in the game on both machines
        deadline = time.monotonic() + 90
        position = re.compile(r"network test: tick \d+ player 0: \(")
        while time.monotonic() < deadline:
            assert games["host"].poll() is None, host_log.read_text(errors="replace")[-4000:]
            if (len(position.findall(join_log.read_text(errors="replace"))) >= 5 and
                    "server: Player joined" in host_log.read_text(errors="replace")):
                break
            time.sleep(1)
        host = host_log.read_text(errors="replace")
        join = join_log.read_text(errors="replace")
        assert "server: the game starts: slayer on bloodgulch" in host, host[-4000:]
        debug = tmp_path / "join" / "data" / "debug.txt"
        assert "server: Player joined" in host, f"no player joined the server:\n{host[-4000:]}\n" \
            f"the client's log:\n{join[-4000:]}\nits debug.txt:\n" \
            f"{debug.read_text(errors='replace')[-3000:] if debug.is_file() else ''}"
        assert len(position.findall(join)) >= 5, f"the client's player never played:\n{join[-4000:]}"
        assert "SDL_Init failed" not in host, host[-4000:]
        # sleeping between frames: a fraction of a core (a busy runner gets a margin)
        stat = Path(f"/proc/{games['host'].pid}/stat")
        before = sum(int(field) for field in stat.read_text().rsplit(")", 1)[1].split()[11:13])
        time.sleep(5)
        after = sum(int(field) for field in stat.read_text().rsplit(")", 1)[1].split()[11:13])
        cpu = (after - before) / os.sysconf("SC_CLK_TCK") / 5
        print(f"the dedicated server used {cpu:.0%} of a core")
        assert cpu < 0.5, f"the dedicated server used {cpu:.0%} of a core"
        games["host"].terminate()
        assert games["host"].wait(timeout=15) == 0
        host = host_log.read_text(errors="replace")
        assert "quitting on signal 15" in host and "server: closed; the players were told" in host, host[-4000:]
        # the client is told and leaves the game
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            debug = tmp_path / "join" / "data" / "debug.txt"
            text = debug.read_text(errors="replace") if debug.is_file() else ""
            # (its connection closing, or the message saying so)
            told = "the game host went down" in text or "the game host is shutting down" in text
            if told:
                break
            time.sleep(1)
        assert told, (tmp_path / "join" / "data" / "debug.txt").read_text(errors="replace")[-4000:]
    finally:
        # (a game still running when the test fails tells where it is: the
        # client left running at the end of a test that passed is stopped)
        failing = sys.exc_info()[0] is not None
        for name, process in games.items():
            if process.poll() is None:
                if failing:
                    _stall_backtraces(name, process, game, tmp_path / name / "run.log")
                process.kill()
            process.wait()


def test_dedicated_server_after_a_player_leaves(tmp_path):
    """A dedicated server goes on hosting after a player leaves its game: a
    client joins the game in progress and plays, and is stopped (as a player
    who quits or loses their connection), and a second client then joins the
    same game and plays. Before, the server's own machine, which has no player,
    took the first player's quit for its own last player's and left the game:
    the server stopped hosting ("no local players remain in the game", then
    "no network game server") until it was restarted. Needs HALO_TEST_GAME (or
    HALO_TEST_SERVER for the server) and the Halo maps
    (HALO_SKATE_TEST_HALO_MAPS); about a minute and a half."""
    game = os.environ.get("HALO_TEST_GAME")
    server = os.environ.get("HALO_TEST_SERVER") or game
    maps = os.environ.get("HALO_SKATE_TEST_HALO_MAPS")
    if not (game and maps and Path(game).is_file() and Path(server).is_file() and
            Path(maps, "bloodgulch.map").is_file()):
        pytest.skip("needs HALO_TEST_GAME and HALO_SKATE_TEST_HALO_MAPS")
    # (the second client at the first's address, which the server answers the
    # search for games at, once the first has gone)
    addresses = {"host": _test_address(214), "first": _test_address(215), "second": _test_address(215)}
    games = {}

    def start(name, settings):
        folder = tmp_path / name
        (folder / "data").mkdir(parents=True)
        (folder / "saves").mkdir()
        (folder / "data" / "maps").symlink_to(maps)
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("SDL_", "HALO_", "DISPLAY", "WAYLAND"))}
        env.update(settings, HALO_NET_ONLINE="false", HALO_UPDATE_ANSWER="never", HALO_NET_ADDRESS=addresses[name],
                   HALO_NET_BROADCAST=addresses["host"] if name != "host" else addresses["first"],
                   HALO_DATA_ROOT=str(folder / "data"), HALO_SAVE_ROOT=str(folder / "saves"))
        with open(folder / "run.log", "w") as log:
            games[name] = subprocess.Popen([server if name == "host" else game], cwd=folder, env=_game_env(env),
                                           stdout=log, stderr=subprocess.STDOUT)

    def log(name):
        return (tmp_path / name / "run.log").read_text(errors="replace")

    def wait_for(condition, seconds, what):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            assert games["host"].poll() is None, f"the server stopped:\n{log('host')[-4000:]}"
            if condition():
                return
            time.sleep(1)
        raise AssertionError(f"{what}:\n{log('host')[-4000:]}")

    position = re.compile(r"network test: tick \d+ player \d+: \(")
    client = {"HALO_NETWORK_TEST": "join", "HALO_NULL_RENDERER": "1", "HALO_NO_AUDIO": "1", "HALO_TEST_INPUT": "bot:7"}
    try:
        start("host", {"HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "5"})
        start("first", client)
        wait_for(lambda: len(position.findall(log("first"))) >= 5 and "server: Player joined" in log("host"), 90,
                 "the first client never played")
        games["first"].kill()
        games["first"].wait()
        wait_for(lambda: "server: Player left (0 players)" in log("host"), 60, "the server never saw the player leave")
        # (the server hosting still: its game goes on, and the second client joins it)
        time.sleep(5)
        host = log("host")
        assert "no local players remain in the game" not in host, host[-4000:]
        assert "no network game server" not in host, host[-4000:]
        start("second", client)
        wait_for(lambda: len(position.findall(log("second"))) >= 5 and log("host").count("server: Player joined") >= 2,
                 90, f"the second client never played; its log:\n{log('second')[-3000:]}")
        assert "no network game server" not in log("host"), log("host")[-4000:]
    finally:
        failing = sys.exc_info()[0] is not None
        for name, process in games.items():
            if process.poll() is None:
                if failing:
                    _stall_backtraces(name, process, server if name == "host" else game, tmp_path / name / "run.log")
                process.kill()
            process.wait()


def test_dedicated_server_skaters(tmp_path):
    """Skate 3 mode on a dedicated server without Skate 3 data: the server
    (HALO_TEST_SERVER: a release build, as the server image's, which has the
    mode but loads nothing of it without the data) has the Halo maps and no
    Skate 3 data, and two clients with the mode
    (HALO_TEST_GAME, HALO_SKATE_TEST_ASSETS) join it. The first skates from 15
    seconds in (debug.network_test_skate); the server steps the second, on
    foot, into their way every five seconds (debug.network_test_skate_block on
    a dedicated server, skate_network.c), and from 55 seconds the second
    skates too. The server relays each skater's state to the other client,
    which poses them with their board; it takes the skaters' places (a board's
    bounds, refusing a stray few at most) and decides each hit
    (skate_authority.c: the skater's report, message 92, and the verdict, 93),
    which both clients hear alike: the second on foot run over, killed by the
    server's damage, the kill the first's. 80 seconds in, the second's
    skater is put under the level (debug.network_test_skate_fall), as a fall
    through a hole in it would: it is put back where it last rode. With a round trip of 150 ms and a tenth of
    the datagrams lost. Skips without the builds, the data and the maps
    (HALO_SKATE_TEST_HALO_MAPS); about two minutes."""
    game = os.environ.get("HALO_TEST_GAME")
    server = os.environ.get("HALO_TEST_SERVER")
    assets = os.environ.get("HALO_SKATE_TEST_ASSETS")
    maps = os.environ.get("HALO_SKATE_TEST_HALO_MAPS")
    if not (game and server and assets and maps and Path(game).is_file() and Path(server).is_file() and
            Path(maps, "bloodgulch.map").is_file()):
        pytest.skip("needs HALO_TEST_GAME, HALO_TEST_SERVER, HALO_SKATE_TEST_ASSETS and HALO_SKATE_TEST_HALO_MAPS")
    seconds = int(os.environ.get("HALO_TEST_SKATE_SECONDS", "110"))
    addresses = {"server": _test_address(214), "first": _test_address(215), "second": _test_address(216)}
    clients = {"HALO_NETWORK_TEST": "join", "HALO_FULLSCREEN": "0", "HALO_NO_AUDIO": "1", "HALO_NULL_RENDERER": "1",
               "HALO_HIDDEN_WINDOW": "1", "HALO_SKATE_ASSETS": assets, "HALO_NETWORK_LATENCY": "75",
               "HALO_NETWORK_LOSS": "10", "HALO_GAME_SEED": "1102", "HALO_NET_BROADCAST": addresses["server"]}
    machines = {"server": {"HALO_DEDICATED": "1", "HALO_NETWORK_TEST_START": "12",
                           "HALO_NETWORK_TEST_SKATE_BLOCK": "1",
                           "HALO_NET_BROADCAST": f"{addresses['first']},{addresses['second']}"},
                "first": dict(clients, HALO_NETWORK_TEST_SKATE="15"),
                # (and 80 seconds in, the second's skater put under the level, as a fall
                # through a hole in it would: put back where it last rode)
                "second": dict(clients, HALO_NETWORK_TEST_SKATE="55", HALO_NETWORK_TEST_SKATE_FALL="80")}
    games = {}
    killed = []
    try:
        for name, settings in machines.items():
            folder = tmp_path / name
            (folder / "data").mkdir(parents=True)
            (folder / "saves").mkdir()
            (folder / "data" / "maps").symlink_to(maps)
            # (the server's environment has no Skate 3 data, nor a display)
            env = {key: value for key, value in os.environ.items()
                   if not key.startswith(("HALO_", "DISPLAY", "WAYLAND")) and
                   (name != "server" or not key.startswith("SDL_"))}
            env.update(settings, HALO_NET_ONLINE="false", HALO_UPDATE_ANSWER="never", HALO_EXIT_AFTER=str(seconds),
                       HALO_NET_ADDRESS=addresses[name], HALO_DATA_ROOT=str(folder / "data"),
                       HALO_SAVE_ROOT=str(folder / "saves"))
            with open(folder / "run.log", "w") as log:
                games[name] = subprocess.Popen([server if name == "server" else game], cwd=folder, env=_game_env(env),
                                               stdout=log, stderr=subprocess.STDOUT)
            time.sleep(4 if name == "server" else 2)
        deadline = time.monotonic() + seconds + 30
        for process in games.values():
            try:
                process.wait(timeout=max(1.0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                pass
    finally:
        for name, process in games.items():
            if process.poll() is None:
                _stall_backtraces(name, process, server if name == "server" else game, tmp_path / name / "run.log")
                process.kill()
                killed.append(name)
            process.wait()
    logs = {name: (tmp_path / name / "run.log").read_text(errors="replace") for name in machines}
    _assert_games_ended(killed, logs, seconds, tmp_path)
    host, first, second = logs["server"], logs["first"], logs["second"]
    assert "server: the game starts: slayer on bloodgulch" in host, host[-4000:]
    # (no skate engine on the server: it never loads the mode's data or steps a skater)
    assert "halo-skate" not in host, [line for line in host.splitlines() if "halo-skate" in line][:5]
    own = {name: set(re.findall(r"skate network: player (\d+) skates here", logs[name])) for name in ("first", "second")}
    assert len(own["first"]) == 1 and len(own["second"]) == 1 and own["first"] != own["second"], own
    (first_player,), (second_player,) = own["first"], own["second"]
    remote = (r"skate network: player (\d+) skates on another machine at .*?: (posed|not posed,) \d+ nodes, "
              r"(the board drawn|no board), .*?host took (\d+) of its places, refused (\d+)")
    seen = {name: re.findall(remote, logs[name]) for name in machines}
    # the server relays: it has each client's skater from their state, and
    # takes their places
    print(f"the server had the skaters {sorted(set(r[0] for r in seen['server']))} for {len(seen['server'])} "
          f"skater-seconds; the first client the second's {len(seen['first'])} s, the second the first's "
          f"{len(seen['second'])} s")
    assert {r[0] for r in seen["server"]} == {first_player, second_player}, host[-4000:]
    taken, refused = int(seen["server"][-1][3]), int(seen["server"][-1][4])
    assert refused <= max(3, taken // 50), f"the server refused {refused} of the skaters' places, took {taken}"
    # each client sees the other skate, posed with their board
    for name, other, least in (("first", second_player, 15), ("second", first_player, 20)):
        lines = [r for r in seen[name] if r[0] == other]
        posed = [r for r in lines if r[1] == "posed" and r[2] == "the board drawn"]
        assert len(lines) >= least, f"the {name} client saw the other skater {len(lines)} s:\n{logs[name][-4000:]}"
        assert len(posed) >= 0.9 * len(lines), f"the {name} client posed them with the board {len(posed)} of {len(lines)} s"
    # and hears the other skate: the server, which plays none, passes each
    # skater's sounds on (skate_network.c, messages 94 and 95)
    _assert_remote_skate_sounds({"first": first, "second": second}, {"first": "second", "second": "first"})
    # a skater under the level (a fall through a hole in it: the second's
    # once fell on and on, at z -300 by the end, and made no sound) is put
    # back on the board where it last rode, and rides on there
    assert "skate test: the skater put under the level" in second, second[-4000:]
    rescued = re.search(r"skate: this machine's skater fell out of the level at .*?: back on the board at "
                        r"\((-?[\d.]+) (-?[\d.]+) (-?[\d.]+)\)", second)
    assert rescued, f"the second's skater was not put back from under the level:\n{second[-4000:]}"
    after = [float(z) for z in re.findall(r"skates here at \(-?[\d.]+ -?[\d.]+ (-?[\d.]+)\)",
                                          second[rescued.end():])]
    print(f"put back at {rescued.groups()}, then {len(after)} seconds skating, lowest at {min(after or [0.0]):.2f}")
    assert len(after) >= 5 and min(after) > float(rescued.group(3)) - 5.0, f"it did not ride on from there: {after}"
    # the server steps the second client's player into the first's way: the
    # first's skater hits them, tells the server, and the server decides
    blocks = re.findall(r"the watcher steps into the skater's way at", host)
    hits = re.findall(r"skate hit \d+: this machine's skater \(player \d+\) hit a person on foot", first)
    decided = dict((int(n), text) for n, text in re.findall(r"skate verdict (\d+) decided: (.*)", host))
    heard = {name: dict((int(n), text) for n, text in re.findall(r"skate verdict (\d+) heard: (.*)", logs[name]))
             for name in ("first", "second")}
    print(f"{len(blocks)} blocks; {len(hits)} hits told; verdicts decided {decided}; heard {heard}")
    assert len(blocks) >= 2, f"the server stepped the player in {len(blocks)} times:\n{host[-4000:]}"
    assert hits, f"the skater never hit the player on foot:\n{first[-4000:]}"
    assert any("on foot" in text for text in decided.values()), f"no hit decided by the server: {decided}"
    for name in ("first", "second"):
        missing = [n for n in decided if n not in heard[name] and n != max(decided)]
        assert not missing, f"the {name} client never heard verdicts {missing}: {decided}, {heard[name]}"
        assert all(heard[name][n] == decided.get(n) for n in heard[name]), f"verdicts differ: {decided}, {heard}"
    # the first's skater riding fast into the second's player on foot runs
    # them over: the server, with no skate engine, decides it and deals the
    # damage, and every machine has the kill the first's
    _assert_run_over(decided, host, first, first_player, {"server": host, "first": first, "second": second})


def test_dedicated_server_rotation(tmp_path):
    """A dedicated server through two turns of its map list, as the server
    image is run (server.maps "bloodgulch:slayer, bloodgulch:ctf,
    hangemhigh:king", one-minute games, no score limit, nobody on it): each
    game ends on its time, its scores are shown, and the next map's game
    starts, till the third, and SIGTERM ends it with status 0. Before, it
    crashed as each game ended: the scores drawn for nobody asked for their
    buttons' high-res icons, and without a window OpenCE has no OpenGL to make
    them with (hud_hires.c). Each game is set up once (the next was set up
    over the one ending, twice, before its server left it), and the last
    profile, which a server has none of, is looked for once, not every frame.
    Its data root is read-only, as the image mounts it, and left untouched:
    its log (debug.txt, on the standard error too), gamestate.txt and
    custom_maps are in its save root.
    The server is HALO_TEST_SERVER's (the image's build), else
    HALO_TEST_GAME's, with the Halo maps (HALO_SKATE_TEST_HALO_MAPS); it skips
    without them. About three minutes and a half."""
    server = os.environ.get("HALO_TEST_SERVER") or os.environ.get("HALO_TEST_GAME")
    maps = os.environ.get("HALO_SKATE_TEST_HALO_MAPS")
    if not (server and maps and Path(server).is_file() and Path(maps, "bloodgulch.map").is_file() and
            Path(maps, "hangemhigh.map").is_file()):
        pytest.skip("needs HALO_TEST_SERVER or HALO_TEST_GAME, and HALO_SKATE_TEST_HALO_MAPS")
    folder = tmp_path / "server"
    (folder / "data").mkdir(parents=True)
    (folder / "saves").mkdir()
    (folder / "data" / "maps").symlink_to(maps)
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("SDL_", "HALO_", "DISPLAY", "WAYLAND"))}
    env.update(HALO_DEDICATED="1", HALO_NETWORK_TEST_START="5", HALO_NET_ONLINE="false",
               HALO_UPDATE_ANSWER="never", HALO_NET_ADDRESS=_test_address(217), HALO_NET_BROADCAST=_test_address(218),
               HALO_SERVER_MAPS="bloodgulch:slayer, bloodgulch:ctf, hangemhigh:king",
               HALO_SERVER_GAME_TYPES="slayer", HALO_SERVER_TIME_LIMIT="1", HALO_SERVER_SCORE_LIMIT="0",
               HALO_DATA_ROOT=str(folder / "data"), HALO_SAVE_ROOT=str(folder / "saves"))
    # (the data root read-only, as the server image mounts it; a run as root
    # writes it all the same, and the listing below checks it is untouched)
    (folder / "data").chmod(0o555)
    log_path, debug_path = folder / "run.log", folder / "saves" / "debug.txt"
    with open(log_path, "w") as log:
        process = subprocess.Popen([server], cwd=folder, env=_game_env(env), stdout=log, stderr=subprocess.STDOUT)
    try:
        # (two games of a minute, each with its scores and the next one's
        # start, and the third's start: about three minutes)
        deadline = time.monotonic() + 300
        third = "server: the game starts: king on hangemhigh"
        while time.monotonic() < deadline:
            text = log_path.read_text(errors="replace")
            assert process.poll() is None, f"the server stopped with status {process.returncode}:\n{text[-4000:]}"
            if third in text:
                break
            time.sleep(1)
        text = log_path.read_text(errors="replace")
        debug = debug_path.read_text(errors="replace") if debug_path.is_file() else ""
        started = re.findall(r"server: the game starts: (\w+)(?: \(teams\))? on (\w+)", text)
        print(f"games started {started}; {text.count('server: the game is over')} ended")
        assert started[:3] == [("slayer", "bloodgulch"), ("ctf", "bloodgulch"), ("king", "hangemhigh")], text[-4000:]
        assert text.count("server: the game is over") >= 2, text[-4000:]
        assert "network test: the next game (bloodgulch, ctf)" in text, text[-4000:]
        assert "network test: the next game (hangemhigh, king)" in text, text[-4000:]
        # (each game set up once: the first, and once for each next one)
        setups = debug.count("setting up a net game")
        assert setups == len(started), f"{setups} games set up for {len(started)} started:\n{debug[-4000:]}"
        lastprof = debug.count("failed to open 'z:\\lastprof.txt'")
        assert lastprof <= 1, f"the last profile was looked for {lastprof} times"
        # the data root written to not at all: the log (debug.txt), gamestate.txt
        # and custom_maps in the save root, and the log on the standard error
        # too, as the image's log
        written = sorted(path.name for path in (folder / "data").iterdir())
        assert written == ["maps"], f"the server wrote to its data root: {written}"
        assert text.count("setting up a net game") == setups, "the log did not go to the standard error too"
        assert (folder / "saves" / "z" / "gamestate.txt").is_file() and (folder / "saves" / "custom_maps").is_dir()
        process.terminate()
        assert process.wait(timeout=15) == 0, log_path.read_text(errors="replace")[-4000:]
    finally:
        if process.poll() is None:
            if sys.exc_info()[0] is not None:
                _stall_backtraces("server", process, server, log_path)
            process.kill()
        process.wait()
        (folder / "data").chmod(0o755)


def test_dedicated_server_without_maps(tmp_path):
    """A dedicated server whose data root lacks the maps it is to host (no maps
    folder, an empty one, one with ui.map but not the map of its list, a file
    that is no map) exits at once with status 2 and one line saying which maps
    and where (port/linux/src/xbox_files.c), before the game or its network
    starts: its tunnel port is never bound and no debug.txt is written. Before,
    it stopped on the damaged disc screen nobody sees, its port bound, deaf to
    SIGTERM. Needs only a Linux build (HALO_TEST_GAME, else build/linux/halo),
    no game data; a second or two."""
    game = os.environ.get("HALO_TEST_GAME") or "build/linux/halo"
    if not Path(game).is_file():
        pytest.skip("needs a Linux build (HALO_TEST_GAME, or ninja linux's build/linux/halo)")
    game = str(Path(game).resolve())
    # (a map's header and no more: what the check reads, signatures and the
    # Xbox caches' version, 5)
    header = bytearray(0x800)
    header[0:4], header[4:8], header[0x7FC:0x800] = b"daeh", (5).to_bytes(4, "little"), b"toof"
    cases = {
        "no maps folder": ({}, "no usable maps in {root}/maps (looked for ui.map, bloodgulch.map); exiting", ""),
        "an empty maps folder": ({"maps": None}, "no usable maps in {root}/maps (looked for ui.map, bloodgulch.map); "
                                 "exiting", ""),
        "a map of the list missing": ({"maps/UI.MAP": bytes(header)}, "maps missing or unusable in {root}/maps: "
                                      "Hangemhigh.map (missing); exiting", "bloodgulch, Hangemhigh:ctf"),
        "no map": ({"maps/ui.map": bytes(header), "maps/bloodgulch.map": b"not a map" * 300},
                   "maps missing or unusable in {root}/maps: bloodgulch.map (not a map); exiting", ""),
    }
    # (a port no other test uses, which the server binds as soon as its
    # network starts: internet play is on, without STUN, which a CI runner
    # may not reach; HALO_TEST_TUNNEL_PORT another, for a second run of the
    # tests beside the first)
    port = int(os.environ.get("HALO_TEST_TUNNEL_PORT", "27315"))
    for index, (name, (files, expected, maps)) in enumerate(cases.items()):
        root = tmp_path / f"data{index}"
        root.mkdir()
        if files == {"maps": None}:
            (root / "maps").mkdir()
        for relative, content in files.items():
            if content is not None:
                (root / relative).parent.mkdir(parents=True, exist_ok=True)
                (root / relative).write_bytes(content)
        if maps:
            (root / "maps" / "bloodgulch.map").write_bytes(bytes(header))
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("SDL_", "HALO_", "DISPLAY", "WAYLAND"))}
        env.update(HALO_DEDICATED="1", HALO_DATA_ROOT=str(root), HALO_SAVE_ROOT=str(tmp_path / f"saves{index}"),
                   HALO_NET_ONLINE="true", HALO_NET_TUNNEL_PORT=str(port), HALO_UPDATE_ANSWER="never",
                   HALO_NET_STUN="")
        if maps:
            env["HALO_SERVER_MAPS"] = maps
        # (in a folder of its own: config.toml is written beside the
        # executable, or in the current folder when that cannot be)
        folder = tmp_path / f"run{index}"
        folder.mkdir()
        started = time.monotonic()
        process = subprocess.Popen([game], cwd=folder, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True)
        bound = False
        try:
            # its port watched for as long as it runs
            while process.poll() is None and time.monotonic() - started < 10:
                for table in ("udp", "udp6"):
                    try:
                        text = Path(f"/proc/{process.pid}/net/{table}").read_text()
                    except OSError:
                        text = ""
                    bound = bound or f":{port:04X} " in text
                time.sleep(0.005)
            status = process.wait(timeout=5)
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
        seconds = time.monotonic() - started
        stderr = process.stderr.read()
        print(f"{name}: status {status} after {seconds:.2f} s: {stderr.strip().splitlines()[-1:]}")
        assert status == 2, f"{name}: status {status}:\n{stderr[-3000:]}"
        assert seconds < 5, f"{name}: took {seconds:.1f} s"
        assert f"halo-linux: server: {expected.format(root=root)}\n" in stderr, f"{name}:\n{stderr[-3000:]}"
        assert not bound, f"{name}: the tunnel port was bound"
        assert "Internet play" not in stderr, f"{name}: the network started:\n{stderr[-3000:]}"
        assert not (root / "debug.txt").exists(), f"{name}: the game started (it wrote debug.txt)"
        assert not (tmp_path / f"saves{index}" / "debug.txt").exists(), f"{name}: the game started (it wrote debug.txt)"
