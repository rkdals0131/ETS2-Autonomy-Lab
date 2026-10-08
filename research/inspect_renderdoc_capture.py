"""Run in RenderDoc 1.46: open an RDC, Window > Python Scripting > Load > Run.

Exports the captured frame's action tree, texture descriptions, and API callstacks.
Does not inject into or change the running game, replay event, or capture file.
"""
import json
from pathlib import Path

import renderdoc as rd


def inspect(controller):
    structured = controller.GetStructuredFile()
    resources = {
        str(resource.resourceId): resource.name
        for resource in controller.GetResources()
    }
    null_resource = rd.ResourceId.Null()

    textures = []
    for texture in controller.GetTextures():
        rid = str(texture.resourceId)
        textures.append({
            "id": rid,
            "name": resources.get(rid, ""),
            "width": texture.width,
            "height": texture.height,
            "depth": texture.depth,
            "format": texture.format.Name(),
            "mips": texture.mips,
            "array_size": texture.arraysize,
            "samples": texture.msSamp,
        })

    actions = []
    events = {}

    def walk(nodes, parents):
        for action in nodes:
            actions.append({
                "event_id": action.eventId,
                "action_id": action.actionId,
                "name": action.GetName(structured),
                "custom_name": action.customName,
                "flags": str(action.flags),
                "fake_marker": action.IsFakeMarker(),
                "parent_event_ids": parents,
                "color_outputs": [
                    {"slot": slot, "resource": str(resource)}
                    for slot, resource in enumerate(action.outputs)
                    if resource != null_resource
                ],
                "depth_output": (
                    str(action.depthOut) if action.depthOut != null_resource else None
                ),
                "event_ids": [event.eventId for event in action.events],
            })
            for event in action.events:
                # RenderDoc's synthetic actions can have no associated API chunk.
                if event.chunkIndex >= len(structured.chunks):
                    continue
                chunk = structured.chunks[event.chunkIndex]
                events[event.eventId] = {
                    "event_id": event.eventId,
                    "chunk_index": event.chunkIndex,
                    "api": chunk.name,
                    "callstack": [hex(address) for address in chunk.metadata.callstack],
                }
            walk(action.children, parents + [action.eventId])

    walk(controller.GetRootActions(), [])
    return {
        "frame_number": controller.GetFrameInfo().frameNumber,
        "api": str(controller.GetAPIProperties().pipelineType),
        "textures": textures,
        "actions": actions,
        "events": list(events.values()),
    }


if not pyrenderdoc.IsCaptureLoaded():
    raise RuntimeError("Open an RDC capture in RenderDoc before running this script.")

result = {}


def collect(controller):
    result.update(inspect(controller))


pyrenderdoc.Replay().BlockInvoke(collect)
output_dir = Path(r"C:\Users\kikiw\Desktop\ETS2-Autonomy-Lab\research\live\2026-10-08-renderdoc")
output_dir.mkdir(parents=True, exist_ok=True)
capture_name = Path(pyrenderdoc.GetCaptureFilename()).stem
output = output_dir / (capture_name + "-inspection.json")
with output.open("w", encoding="utf-8") as stream:
    json.dump(result, stream, ensure_ascii=False, indent=2)
print("Saved", output)
print("Textures:", len(result["textures"]), "Actions:", len(result["actions"]))
