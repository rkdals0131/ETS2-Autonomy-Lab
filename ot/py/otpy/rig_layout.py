"""Use the native mounting authority shared with the Windows relay."""

def resolve_layout(config, client):
    if not any("position_base_link" in view for view in config["views"]) and not config.get("truck_id"):
        return config
    return client.request("resolve_layout", rig=config)
