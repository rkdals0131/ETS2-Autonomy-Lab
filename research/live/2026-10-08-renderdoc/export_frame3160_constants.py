"""Export the captured shaders, constant buffers and viewport for the four views.

Run with qrenderdoc --python. The selected full-screen triangles were found in
frame3160.xml; their shader disassembly determines their roles, not their order.
This replays the saved capture and never attaches to the running game.
"""
import json
import math
import sys
import traceback
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

import renderdoc as rd

ROOT = Path(r"C:\Users\kikiw\Desktop\ETS2-Autonomy-Lab\research\live\2026-10-08-renderdoc")
OUTPUT = ROOT / "constants"
SELECTIONS = [
    ("wide", (11318, 11410, 11746, 11979)),
    ("tall_first", (14840, 15016, 15511, 15748)),
    ("square", (19616, 19775, 20611, 20839)),
    ("tall_second", (23298, 23436, 23930, 24104)),
]
PHASES = ("gbuffer_end", "fullscreen_first", "fullscreen_second", "color_end")
OUTPUT.mkdir(exist_ok=True)


def variable_value(variable):
    item = {"name": variable.name, "type": str(variable.type),
            "rows": variable.rows, "columns": variable.columns}
    if variable.members:
        item["members"] = [variable_value(member) for member in variable.members]
    else:
        count = variable.rows * variable.columns
        if variable.type == rd.VarType.Float:
            item["values"] = list(variable.value.f32v)[:count]
        elif variable.type == rd.VarType.Double:
            item["values"] = list(variable.value.f64v)[:count]
        elif variable.type == rd.VarType.SInt:
            item["values"] = list(variable.value.s32v)[:count]
        elif variable.type in (rd.VarType.UInt, rd.VarType.Bool):
            item["values"] = list(variable.value.u32v)[:count]
        else:
            # Preserve uninterpreted values; the raw constant buffer is also saved.
            item["raw_u32_words"] = list(variable.value.u32v)
        if "values" in item:
            # Stripped reflection exposes unused lanes too, including NaN bit patterns.
            item["values"] = [str(value) if isinstance(value, float) and not math.isfinite(value)
                              else value for value in item["values"]]
    return item


def export(controller):
    if controller.GetFrameInfo().frameNumber != 3160:
        raise RuntimeError("This selection belongs to frame3160.rdc")
    wanted = {chunk for _, chunks in SELECTIONS for chunk in chunks}
    events = {}

    def visit(actions):
        for action in actions:
            for event in action.events:
                if event.chunkIndex in wanted:
                    events[event.chunkIndex] = event.eventId
            visit(action.children)

    visit(controller.GetRootActions())
    result = {"frame_number": 3160, "events": []}
    shader_files = {}
    for view, chunks in SELECTIONS:
        for phase, chunk in zip(PHASES, chunks):
            event = events[chunk]
            controller.SetFrameEvent(event, True)
            pipe = controller.GetPipelineState()
            viewport = pipe.GetViewport(0)
            item = {
                "view_candidate": view, "selection": phase,
                "chunk_index": chunk, "event_id": event,
                "viewport": {key: getattr(viewport, key) for key in
                             ("x", "y", "width", "height", "minDepth", "maxDepth")},
                "outputs": [str(target.resource) for target in pipe.GetOutputTargets()],
                "stages": {},
            }
            for stage_name, stage in (("vs", rd.ShaderStage.Vertex), ("ps", rd.ShaderStage.Pixel)):
                reflection = pipe.GetShaderReflection(stage)
                if reflection is None:
                    continue
                shader = pipe.GetShader(stage)
                shader_key = str(shader)
                if shader_key not in shader_files:
                    filename = "shader_" + str(len(shader_files)) + "_" + stage_name + ".txt"
                    (OUTPUT / filename).write_text(controller.DisassembleShader(
                        pipe.GetGraphicsPipelineObject(), reflection, ""), encoding="utf-8")
                    shader_files[shader_key] = filename
                stage_data = {"shader": shader_key, "disassembly": shader_files[shader_key],
                              "read_only_resources": [str(bound.descriptor.resource) for bound
                                                      in pipe.GetReadOnlyResources(stage, True)],
                              "constant_blocks": []}
                for index, block in enumerate(reflection.constantBlocks):
                    descriptor = pipe.GetConstantBlock(stage, index, 0).descriptor
                    variables = controller.GetCBufferVariableContents(
                        pipe.GetGraphicsPipelineObject(), shader, stage,
                        pipe.GetShaderEntryPoint(stage), index, descriptor.resource,
                        descriptor.byteOffset, descriptor.byteSize)
                    buffer_file = None
                    if block.bufferBacked and descriptor.resource != rd.ResourceId.Null():
                        buffer_file = "{}_{}_{}_cb{}.bin".format(view, phase, stage_name, index)
                        (OUTPUT / buffer_file).write_bytes(controller.GetBufferData(
                            descriptor.resource, descriptor.byteOffset, block.byteSize))
                    stage_data["constant_blocks"].append({
                        "name": block.name, "index": index,
                        "d3d_register": block.fixedBindNumber,
                        "resource": str(descriptor.resource), "byte_offset": descriptor.byteOffset,
                        "byte_size": block.byteSize, "raw_file": buffer_file,
                        "variables": [variable_value(variable) for variable in variables],
                    })
                item["stages"][stage_name] = stage_data
            result["events"].append(item)
            print("Saved", view, phase, "event", event, flush=True)
    (OUTPUT / "draw-state.json").write_text(json.dumps(result, indent=2, allow_nan=False), encoding="utf-8")
    print("Finished", OUTPUT / "draw-state.json", flush=True)


def run_headless():
    capture = rd.OpenCaptureFile()
    controller = None
    try:
        result = capture.OpenFile(str(ROOT / "frame3160.rdc"), "rdc", None)
        if not result.OK():
            raise RuntimeError(str(result))
        result, controller = capture.OpenCapture(rd.ReplayOptions(), None)
        if not result.OK():
            raise RuntimeError(str(result))
        export(controller)
    finally:
        if controller is not None:
            controller.Shutdown()
        capture.Shutdown()


def run_loaded(controller):
    previous_event = pyrenderdoc.CurEvent()
    try:
        export(controller)
    finally:
        controller.SetFrameEvent(previous_event, True)


gui_loaded = "pyrenderdoc" in globals() and pyrenderdoc.IsCaptureLoaded()
exit_code = 0
with (OUTPUT / "export.log").open("w", encoding="utf-8", buffering=1) as log:
    with redirect_stdout(log), redirect_stderr(log):
        try:
            if gui_loaded:
                pyrenderdoc.Replay().BlockInvoke(run_loaded)
            else:
                run_headless()
        except Exception:
            traceback.print_exc()
            exit_code = 1
if not gui_loaded:
    sys.exit(exit_code)
