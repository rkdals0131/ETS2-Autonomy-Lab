"""RenderDoc Python Scripting: extract selected draw results from frame3160.rdc.

Selections come from frame3160-target-bindings.json (RenderDoc XML chunk indices,
not event IDs). Candidate names describe render order/size, not confirmed mirrors.
"""
import json
import sys
import traceback
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path

import renderdoc as rd

OUTPUT = Path(r"C:\Users\kikiw\Desktop\ETS2-Autonomy-Lab\research\live\2026-10-08-renderdoc\pixels")
SELECTIONS = [
    ("wide", 11318, 11979),
    ("tall_first", 14840, 15748),
    ("square", 19616, 20839),
    ("tall_second", 23298, 24104),
]

OUTPUT.mkdir(parents=True, exist_ok=True)


def export(controller, previous_event=None):
    frame_number = controller.GetFrameInfo().frameNumber
    if frame_number != 3160:
        raise RuntimeError("These draw selections belong to frame3160.rdc; open that capture first.")
    wanted = {chunk for _, gbuffer, color in SELECTIONS for chunk in (gbuffer, color)}
    events = {}
    textures = {str(texture.resourceId): texture for texture in controller.GetTextures()}

    def visit(actions):
        for action in actions:
            for event in action.events:
                if event.chunkIndex in wanted:
                    events[event.chunkIndex] = event.eventId
            visit(action.children)

    visit(controller.GetRootActions())
    metadata = {"frame_number": frame_number, "images": []}

    def save(label, chunk, slot):
        event = events[chunk]
        controller.SetFrameEvent(event, True)
        resource = controller.GetPipelineState().GetOutputTargets()[slot].resource
        texture = textures[str(resource)]
        subresource = rd.Subresource()
        subresource.mip = 0
        subresource.slice = 0
        subresource.sample = 0
        pixels = controller.GetTextureData(resource, subresource)
        path = OUTPUT / (label + ".bin")
        path.write_bytes(pixels)
        metadata["images"].append({
            "file": path.name,
            "chunk_index": chunk,
            "event_id": event,
            "output_slot": slot,
            "resource": str(resource),
            "width": texture.width,
            "height": texture.height,
            "format": texture.format.Name(),
            "component_count": texture.format.compCount,
            "component_bytes": texture.format.compByteWidth,
            "component_type": str(texture.format.compType),
            "byte_count": len(pixels),
        })
        print("Saved", label, "at event", event, "bytes", len(pixels))

    try:
        for name, gbuffer, color in SELECTIONS:
            save(name + "_attributes0", gbuffer, 0)
            save(name + "_attributes3", gbuffer, 3)
            save(name + "_color", color, 0)
        (OUTPUT / "images.json").write_text(json.dumps(metadata, indent=2), encoding="utf-8")
    finally:
        if previous_event is not None:
            controller.SetFrameEvent(previous_event, True)


def run_headless():
    capture = rd.OpenCaptureFile()
    controller = None
    try:
        source = OUTPUT.parent / "frame3160.rdc"
        print("Opening", source, flush=True)
        result = capture.OpenFile(str(source), "rdc", None)
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


if "pyrenderdoc" in globals() and pyrenderdoc.IsCaptureLoaded():
    previous_event = pyrenderdoc.CurEvent()
    pyrenderdoc.Replay().BlockInvoke(lambda controller: export(controller, previous_event))
else:
    # --python also supplies pyrenderdoc, but no capture is loaded at startup.
    # qrenderdoc owns replay initialisation; SystemExit prevents its main UI loop.
    exit_code = 0
    with (OUTPUT / "export.log").open("w", encoding="utf-8", buffering=1) as log:
        with redirect_stdout(log), redirect_stderr(log):
            try:
                run_headless()
            except Exception:
                traceback.print_exc()
                exit_code = 1
    sys.exit(exit_code)
