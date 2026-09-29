#!/usr/bin/env python3
"""Every argument a Claude tool reads is in its schema, and the other way round.

Qucs-S refuses a tool call with an argument its schema lacks (QucsControl::
unknownArguments), so an argument a handler reads but its schema leaves out
cannot be given at all: replace_component's 'records' and redo's 'to' were
lost that way. And one in the schema that nothing reads is taken and left
out without a word.

This reads qucs-s-26.1.1/qucs/qucscontrol*.cpp: the tools' schemas (kTools
and the two added in the constructor), call()'s dispatch from each tool to
its handler, and every key each handler reads - args.value(QLatin1String(
"k")), args.contains(...), a lambda's or a table's keys - following args
into the functions it is handed to. It also checks the objects inside an
argument whose schema describes them ("items" with "properties": a
set_schematic component, a wire, a trace, a batch call) against the
function that reads one.

    python3 scripts/ci/check-tool-arguments.py

Exits 1, and says which, when a tool and its schema disagree.
"""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "..", "qucs-s-26.1.1", "qucs")
FILES = sorted(f for f in os.listdir(SOURCE) if re.fullmatch(r"qucscontrol(_\w+)?\.cpp", f))

# Read by call() or the machinery around it, not by a handler.
AROUND = {"preview", "selection"}
# Arguments a handler reads on purpose though its schema has them not
# (none), and schema fields no handler is expected to read, by tool.
NOT_READ = {
    # get_state's path is put in by a conversation pinned to a schematic.
    ("get_state", "path"),
}
# Read by a handler another tool shares, for that tool: get_netlist drops
# them (export_netlist writes the netlist, and whether over a file).
OTHERS = {("get_netlist", "save_as"), ("get_netlist", "replace"),
          # (and the map and numbering, which only get_netlist shows)
          ("export_netlist", "map"), ("export_netlist", "numbered"),
          # (undo's alone: redo refuses it, as its schema has it not)
          ("redo", "files")}
# Nested objects: the argument whose items are read by a function, and that
# function's parameter.
NESTED = {
    ("set_schematic", "components"): ("componentLineOf", "o"),
    ("set_schematic", "wires"): ("wireLineOf", "o"),
    ("add_diagram", "traces"): ("addDiagram", "t"),
    ("batch", "calls"): ("next", "call"),
    ("delete", "traces"): ("remove", "t"),
    ("set_dialog", "set"): ("setDialog", "change"),
    ("tune", "knobs"): ("tuneKnobs", "knob"),
    ("tune", "targets"): ("tuneKnobs", "item"),
    ("tune", "hold"): ("readHolds", "item"),
}


def source():
    return {f: open(os.path.join(SOURCE, f), encoding="utf-8").read() for f in FILES}


def schemas(text):
    """Tool name -> (properties, additionalProperties, items' properties by argument)."""
    main = text["qucscontrol.cpp"]
    blocks = [main[main.index('kTools = R"JSON(') + len('kTools = R"JSON('):main.index(')JSON";')]]
    tools = json.loads(blocks[0])
    for m in re.finditer(r'fromJson\(R"JSON\((\{"name".*?)\)JSON"\)', main, re.S):
        tools.append(json.loads(m.group(1)))
    previewable = re.search(r"kPreviewable\[\] = \{([^}]*)\}", main).group(1)
    previewable = set(re.findall(r'"(\w+)"', previewable))
    out = {}
    for t in tools:
        schema = t["inputSchema"]
        props = set(schema.get("properties", {}))
        if t["name"] in previewable:
            props.add("preview")
        items = {}
        for k, v in schema.get("properties", {}).items():
            it = v.get("items") if isinstance(v, dict) else None
            # (Text or an object: the object's.)
            for alt in (it or {}).get("anyOf", []) if isinstance(it, dict) else []:
                if "properties" in alt:
                    it = alt
            if isinstance(it, dict) and "properties" in it:
                items[k] = (set(it["properties"]), it.get("additionalProperties", False))
        out[t["name"]] = (props, schema.get("additionalProperties", False), items)
    return out


def functions(text):
    """Function name -> list of (parameter names, body)."""
    out = {}
    head = re.compile(r"^[\w:<>*&, ]*?\b(?:QucsControl::)?(\w+)\(([^;{}]*?)\)\s*(?:const\s*)?(?:->\s*[\w:<>]+\s*)?\{", re.M | re.S)
    for body_text in text.values():
        for m in head.finditer(body_text):
            name = m.group(1)
            if name in ("if", "for", "while", "switch", "catch", "return"):
                continue
            params = []
            depth = 0
            cur = ""
            for ch in m.group(2):
                if ch in "<([":
                    depth += 1
                elif ch in ">)]":
                    depth -= 1
                if ch == "," and depth == 0:
                    params.append(cur)
                    cur = ""
                else:
                    cur += ch
            if cur.strip():
                params.append(cur)
            names = []
            for p in params:
                p = p.split("=")[0].strip()
                w = re.findall(r"\w+", p)
                names.append(w[-1] if w else "")
            start = m.end() - 1
            depth, i = 0, start
            while i < len(body_text):
                if body_text[i] == "{":
                    depth += 1
                elif body_text[i] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            out.setdefault(name, []).append((names, body_text[start:i + 1]))
    return out


def keys_read(funcs, name, param, seen=None):
    """The keys function \a name reads of its parameter \a param, following it on."""
    seen = set() if seen is None else seen
    if (name, param) in seen:
        return set()
    seen.add((name, param))
    keys = set()
    for names, body in funcs.get(name, []):
        if param not in names:
            continue
        keys |= keys_in(funcs, body, param, seen)
    return keys


def keys_in(funcs, body, param, seen):
    keys = set()
    aliases = {param}
    # A copy: QJsonObject looking = args; - what it is given or loses
    # there is not the caller's.
    put = set()
    for m in re.finditer(r"QJsonObject (\w+) = %s;" % re.escape(param), body):
        aliases.add(m.group(1))
        put |= set(re.findall(r"\b%s\.(?:insert|remove)\(Q(?:Latin1String|StringLiteral)\(\"([^\"]+)\"\)" % re.escape(m.group(1)), body))
    for p in aliases:
        q = re.escape(p)
        keys |= set(re.findall(r"\b%s(?:\.value|\.contains|\.constFind|\.find|\.take)\(Q(?:Latin1String|StringLiteral)\(\"([^\"]+)\"\)" % q, body)) - (put if p != param else set())
        keys |= set(re.findall(r"\b%s\[Q(?:Latin1String|StringLiteral)\(\"([^\"]+)\"\)\]" % q, body))
        # A key held in a variable: a lambda's (given("from", ...)) or a
        # table's ({"x_axis", ...}, for (const char* other : {...})).
        for m in re.finditer(r"\b%s\.(?:value|contains)\(QLatin1String\((\w+)(?:\.(\w+))?\)\)" % q, body):
            var = m.group(2) or m.group(1)
            lam = re.search(r"(?:const )?auto (\w+) = \[[^\]]*\]\([^)]*\b%s\b[^)]*\)[^{]*\{[^}]*?%s" % (re.escape(m.group(1)), re.escape(m.group(0))), body, re.S)
            if lam:
                keys |= set(re.findall(r"\b%s\(\"(\w+)\"" % re.escape(lam.group(1)), body))
                continue
            loop = re.search(r"for \(const char\* %s : \{([^}]*)\}" % re.escape(m.group(1)), body)
            if loop:
                keys |= set(re.findall(r'"(\w+)"', loop.group(1)))
                continue
            table = re.search(r"const char\* %s;.*?\}\s*\w+\[\] = \{(.*?)\};" % re.escape(var), body, re.S)
            if table:
                keys |= set(re.findall(r'\{"(\w+)"', table.group(1)))
                continue
            print(f"  (a key read through {m.group(0)} not followed)")
        # Handed on: f(..., args, ...).
        for callee, args in calls(body):
            for i, a in enumerate(args):
                if a == p and callee in funcs:
                    sub = set()
                    for names, _ in funcs[callee]:
                        if i < len(names):
                            sub |= keys_read(funcs, callee, names[i], seen)
                    keys |= sub - (put if p != param else set())
    return keys


def calls(body):
    """Each call in \a body: its function's name and its arguments."""
    for m in re.finditer(r"\b(\w+)\(", body):
        depth, i = 1, m.end()
        while i < len(body) and depth:
            if body[i] in "([{":
                depth += 1
            elif body[i] in ")]}":
                depth -= 1
            i += 1
        yield m.group(1), [a.strip() for a in split_args(body[m.end():i - 1])]


def split_args(s):
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch in "<([{":
            depth += 1
        elif ch in ">)]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    out.append(cur)
    return out


def dispatch(text):
    main = text["qucscontrol.cpp"]
    body = main[main.index("QJsonObject QucsControl::call("):]
    body = body[:body.index("\n}\n")]
    out = {}
    for m in re.finditer(r'tool == QLatin1String\("(\w+)"\)\) (?:return )?(\w+)\((\w*)', body):
        out.setdefault(m.group(1), (m.group(2), m.group(3)))
    # Each in a block of its own: get_netlist drops 'save_as' before the
    # handler they share (export_netlist writes it).
    out["get_netlist"] = ("getNetlist", "args")
    out["export_netlist"] = ("getNetlist", "args")
    return out


def main():
    text = source()
    tools = schemas(text)
    funcs = functions(text)
    handlers = dispatch(text)
    problems = []
    for tool, (props, open_ended, items) in sorted(tools.items()):
        if tool == "describe_tool":
            read = {"name"}
        elif tool not in handlers:
            problems.append(f"{tool}: no handler found in call()")
            continue
        else:
            handler, arg = handlers[tool]
            read = keys_read(funcs, handler, "args") if arg == "args" else set()
        read -= AROUND
        # (A painting tool's fields are its type's, read by the painting.)
        if open_ended:
            continue
        for k in sorted(read - props):
            if (tool, k) not in NOT_READ and (tool, k) not in OTHERS:
                problems.append(f"{tool}: reads '{k}', which its schema lacks - a call with it is refused")
        for k in sorted(props - read - AROUND):
            if (tool, k) not in NOT_READ:
                problems.append(f"{tool}: its schema has '{k}', which nothing reads - given, it is left out without a word")
        for arg_name, (item_props, item_open) in items.items():
            where = NESTED.get((tool, arg_name))
            if where is None:
                problems.append(f"{tool}: '{arg_name}' describes its items, but this script does not know what reads them (NESTED)")
                continue
            nested = set()
            for _, body in funcs.get(where[0], []):
                nested |= keys_in(funcs, body, where[1], set())
            if not item_open:
                for k in sorted(nested - item_props):
                    problems.append(f"{tool}: a '{arg_name}' item's '{k}' is read but not in the item schema")
            for k in sorted(item_props - nested):
                problems.append(f"{tool}: a '{arg_name}' item's '{k}' is in the item schema but nothing reads it")
    for p in problems:
        print(p)
    print(f"{len(tools)} tools checked, {len(problems)} disagreements")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
