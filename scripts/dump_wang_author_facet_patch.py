"""LLDB extension: dump the stopped author's recoverFacebyaddinSt patch.

Load only in the author diagnostic process, stopped at dt.cpp:2308:
  command script import scripts/dump_wang_author_facet_patch.py
  dump_wang_author_facet_patch

This reads debugger state; it never calls or links the author code from the
owned implementation.
"""

import lldb


def _value(frame, expression):
    value = frame.EvaluateExpression(expression)
    if not value.IsValid() or value.GetError().Fail():
        raise RuntimeError(f"cannot evaluate {expression}: {value.GetError()}")
    return value


def _integer(frame, expression):
    return _value(frame, expression).GetValueAsSigned()


def _scalar(frame, expression):
    return _value(frame, expression).GetValue()


def dump_wang_author_facet_patch(debugger, command, result, internal_dict):
    del debugger, command, internal_dict
    frame = lldb.debugger.GetSelectedTarget().process.GetSelectedThread().GetSelectedFrame()
    target = _integer(frame, "targetF")
    vertices = [_integer(frame, f"SurTris[{target}].form[{index}]")
                for index in range(3)]
    pending = _value(frame, "patch.pending")
    slots = [pending.GetChildAtIndex(index).GetValueAsSigned()
             for index in range(pending.GetNumChildren())]
    result.PutCString("facet_patch_target {} {} {} {}".format(target, *vertices))
    nodes = set(vertices)
    for slot in slots:
        cell = [_integer(frame, f"Elems[{slot}].form[{corner}]")
                for corner in range(4)]
        nodes.update(cell)
        result.PutCString("facet_patch_cell {} {} {} {} {}".format(slot, *cell))
    for node in sorted(nodes):
        point = [_scalar(frame, f"Nodes[{node}].pt[{axis}]")
                 for axis in range(3)]
        carrier = _integer(frame, f"getP2T({node})")
        result.PutCString("facet_patch_node {} {} {} {} p2t {}".format(
            node, *point, carrier))
    edges = _value(frame, "patch.edges")
    for index in range(edges.GetNumChildren()):
        edge = edges.GetChildAtIndex(index)
        result.PutCString("facet_patch_edge {} {}".format(
            edge.GetChildAtIndex(0).GetValueAsSigned(),
            edge.GetChildAtIndex(1).GetValueAsSigned()))


def __lldb_init_module(debugger, internal_dict):
    del internal_dict
    debugger.HandleCommand(
        "command script add -f dump_wang_author_facet_patch.dump_wang_author_facet_patch "
        "dump_wang_author_facet_patch")
