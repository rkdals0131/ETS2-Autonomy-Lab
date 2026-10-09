#include "render_sample.hpp"
#include "../include/geometry.hpp"

namespace ot {
json RenderDrawSample::binding() const {
    return {{"known",known},{"source_buffer",buffer},{"first_constant",first},{"num_constants",count}};
}
json RenderActorPlacement::describe() const {
    return {{"local_xyz",local},{"cell_xz",cells},{"quaternion_wxyz",quaternion},{"world_xyz",world_position(local,cells)}};
}
json RenderVehicleSample::describe() const {
    json result={{"actor_address",actor},{"kind",parked?"parked":"ai"},{"model_address",model},
        {"model_object_address",object},{"lod_index",lod},{"component_address",component},
        {"geometry_addresses",geometry},{"qpc",qpc},{"model_rotation_row_major",rotation},
        {"model_local_xyz",local},{"model_cell_xz",cells},{"model_world_xyz",world_position(local,cells)},
        {"model_reference_offset_raw",reference},
        {"actor_observation",{{"placement",actor_placement.describe()},{"aabb_raw",aabb}}},{"draws",json::array()}};
    for(const auto& draw:draws) result["draws"].push_back({{"draw_item_index",draw.item_index},
        {"geometry_address",draw.geometry},{"qpc",draw.qpc},{"vs_cb0",draw.binding()}});
    return result;
}
json RenderVehiclesSample::describe() const {
    json result={{"available",available},{"qpc_begin",qpc_begin},{"qpc_end",qpc_end},
        {"sample_phase","dx11_compile_pass_begin"},
        {"scope","AI and parked body models in this pass; actor pose is a separate simulation observation"},
        {"vehicles",json::array()},{"errors",json::array()},{"truncated_for_read_budget",truncated},
        {"draw_bindings",{{"sample_phase","after_dx11_draw_binding_preparation"},
            {"scope","emitted draw batch VS slot 0; final draw execution not hooked"},
            {"observed_draw_items",draw_count},{"truncated_for_read_budget",draws_truncated},{"error",draw_error}}}};
    if(scene_observed) result.update({{"scene_address",scene},{"source_geometry_count",source_count},
        {"prepared_group_count",group_count},{"state_override_count",override_count}});
    if(geometry_observed) result.update({{"unique_geometry_count",unique_count},
        {"geometry_scope","prepared_draw_items; per-range dispatch suppression and final draw execution not traced"}});
    if(actors_observed) result["actors_considered"]=actors_considered;
    if(!error.empty()) result["error"]=error;
    for(const auto& [actor,message]:errors) result["errors"].push_back({{"actor_address",actor},{"error",message}});
    for(const auto& vehicle:vehicles) result["vehicles"].push_back(vehicle.describe());
    return result;
}
json RenderCameraSample::describe() const {
    json result={{"available",available},{"sample_phase","dx11_compile_pass_begin"},
        {"qpc",qpc},{"scope","pass_base_state; per-draw overrides not inspected"}};
    if(!available) {result["error"]=error;return result;}
    result.update({{"work_address",work},{"component_batch_id",batch},
        {"component_mask",component_mask},{"camera_address",camera},{"deferred_state_address",deferred},
        {"viewport_depth",viewport_depth},{"viewport_mode",viewport_mode},{"viewport_rect_raw",viewport_rect},
        {"projection_row_major",projection},{"projection_modifier",projection_modifier},
        {"projection_modifier_flag",projection_modifier_flag},{"camera_rotation_row_major",rotation},
        {"camera_local_xyz",local},{"camera_cell_xz",cells},{"camera_world_xyz",world},
        {"world_units","game_length_units"},{"ray",ray},{"deferred_dimensions",dimensions}});
    return result;
}
json RenderPassSample::describe(bool diagnostic) const {
    json result=json::object();
    if(diagnostic) {
        result={{"pass_address",pass},{"command_buffer",input},{"graph_buffer",graph},
            {"pass_name",name},{"pass_namespace",space},{"linked_images",json::array()}};
        for(const auto& image:links) result["linked_images"].push_back({
            {"reference_array_offset",image.reference_offset},{"graph_image_id",image.id},
            {"namespace",image.space},{"name",image.name},{"pool_id_at_compile",image.pool}});
    }
    if(geometry) {
        result["camera_at_compile"]=camera.describe();
        if(vehicles) result["vehicles_at_compile"]=vehicles->describe();
        if(sdk) {
            auto& timing=result["sdk_at_compile"]=json::object();
            for(const auto* key:{"frame_id","truck_generation","paused","render_time_us","simulation_time_us",
                    "paused_simulation_time_us","timer_flags"}) timing[key]=sdk->at(key);
            timing["association"]="last SDK frame_end before pass compilation";
            if(diagnostic) timing["sdk"]=sdk->at("sdk");
            if(sdk->contains("engine") && sdk->at("engine").contains("vehicle"))
                result["ego_at_compile"]=sdk->at("engine").at("vehicle");
        }
    }
    return result;
}
}
