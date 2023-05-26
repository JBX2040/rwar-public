#include <Client/Game.h>

#include <assert.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>

#ifndef EMSCRIPTEN
#include <libwebsockets.h>
#endif

#include <Client/Renderer/Renderer.h>
#include <Client/Renderer/ComponentRender.h>
#include <Client/InputData.h>
#include <Client/Socket.h>
#include <Client/Simulation.h>
#include <Shared/Utilities.h>
#include <Shared/Bitset.h>
#include <Shared/Crypto.h>
#include <Shared/pb.h>
#include <Shared/Component/Arena.h>
#include <Shared/Component/Flower.h>
#include <Shared/Component/Petal.h>
#include <Shared/Component/PlayerInfo.h>
#include <Shared/Component/Physical.h>

void rr_game_init(struct rr_game *this)
{
    memset(this, 0, sizeof *this);
}

void rr_game_websocket_on_event_function(enum rr_websocket_event_type type, void *data, void *captures, uint64_t size)
{
    struct rr_game *this = captures;
    switch (type)
    {
    case rr_websocket_event_type_open:
        puts("websocket opened");
        break;
    case rr_websocket_event_type_close:
        puts("websocket closed");
        break;
    case rr_websocket_event_type_data:
    {
        struct proto_bug encoder;
        proto_bug_init(&encoder, data);

        if (!this->socket->recieved_first_packet)
        {
            this->socket->recieved_first_packet = 1;
            printf("size %lld\n", size);
            rr_decrypt(data, size, 1);
            this->socket->encryption_key = proto_bug_read_uint64(&encoder, "encryption key");
            printf("got key %llu\n", this->socket->encryption_key);
            return;
        }

        this->socket->encryption_key = rr_get_hash(this->socket->encryption_key);
        rr_decrypt(data, size, this->socket->encryption_key);
        switch (proto_bug_read_uint8(&encoder, "header"))
        {
        case 0:
            rr_simulation_read_binary(this->simulation, &encoder);
            break;
        default:
            RR_UNREACHABLE("how'd this happen");
        }
        struct proto_bug encoder2;
        static uint8_t output_packet[128 * 1024];
        proto_bug_init(&encoder2, output_packet);
        proto_bug_write_uint8(&encoder2, 0, "header");
        proto_bug_write_uint8(&encoder2, 0, "movement type");
        uint8_t movement_flags = 0;
        movement_flags |= (rr_bitset_get(this->input_data->keys_pressed, 87) || rr_bitset_get(this->input_data->keys_pressed, 38)) << 0;
        movement_flags |= (rr_bitset_get(this->input_data->keys_pressed, 65) || rr_bitset_get(this->input_data->keys_pressed, 37)) << 1;
        movement_flags |= (rr_bitset_get(this->input_data->keys_pressed, 83) || rr_bitset_get(this->input_data->keys_pressed, 40)) << 2;
        movement_flags |= (rr_bitset_get(this->input_data->keys_pressed, 68) || rr_bitset_get(this->input_data->keys_pressed, 39)) << 3;
        movement_flags |= this->input_data->mouse_buttons << 4;
        movement_flags |= rr_bitset_get(this->input_data->keys_pressed, 32) << 4;
        movement_flags |= rr_bitset_get(this->input_data->keys_pressed, 16) << 5;
        proto_bug_write_uint8(&encoder2, movement_flags, "movement kb flags");
        rr_websocket_send(this->socket, encoder2.start, encoder2.current);
        break;
    }
    default:
        RR_UNREACHABLE("non exhaustive switch expression");
    }
}

void render_health_component(EntityIdx entity, void *_captures)
{
    struct rr_game *this = _captures;
    if (!rr_simulation_has_health(this->simulation, entity))
        return;
    struct rr_renderer_context_state state;
    rr_renderer_init_context_state(this->renderer, &state);
    rr_component_health_render(entity, this->simulation, this->renderer);
    rr_renderer_free_context_state(this->renderer, &state);
}

void render_mob_component(EntityIdx entity, void *_captures)
{
    struct rr_game *this = _captures;
    if (!rr_simulation_has_mob(this->simulation, entity))
        return;
    struct rr_renderer_context_state state;
    rr_renderer_init_context_state(this->renderer, &state);
    rr_component_mob_render(entity, this->simulation, this->renderer);
    rr_renderer_free_context_state(this->renderer, &state);
}

void render_petal_component(EntityIdx entity, void *_captures)
{
    struct rr_game *this = _captures;
    if (!rr_simulation_has_petal(this->simulation, entity))
        return;
    struct rr_renderer_context_state state;
    rr_renderer_init_context_state(this->renderer, &state);
    rr_component_petal_render(entity, this->simulation, this->renderer);
    rr_renderer_free_context_state(this->renderer, &state);
}

void render_flower_component(EntityIdx entity, void *_captures)
{
    struct rr_game *this = _captures;
    if (!rr_simulation_has_flower(this->simulation, entity))
        return;
    struct rr_renderer_context_state state;
    rr_renderer_init_context_state(this->renderer, &state);
    rr_component_flower_render(entity, this->simulation, this->renderer);
    rr_renderer_free_context_state(this->renderer, &state);
}

void rr_game_tick(struct rr_game *this, float delta)
{
    rr_simulation_tick(this->simulation, delta);

    struct timeval start;
    struct timeval end;

    gettimeofday(&start, NULL);

    rr_renderer_set_transform(this->renderer, 1, 0, 0, 0, 1, 0);
    struct rr_renderer_context_state state1;
    struct rr_renderer_context_state state2;
    if (this->player_info == 0)
    {
        for (EntityIdx player_info = 1; player_info < RR_MAX_ENTITY_COUNT; ++player_info)
        {
            if (!rr_bitset_get_bit(this->simulation->player_info_tracker, player_info))
                continue;
            this->player_info = player_info;
            break;
        }
    }
    if (this->player_info != 0)
    {
        rr_renderer_init_context_state(this->renderer, &state1);
        struct rr_component_player_info *player_info = rr_simulation_get_player_info(this->simulation, this->player_info);
        rr_renderer_translate(this->renderer, this->renderer->width / 2, this->renderer->height / 2);
        rr_renderer_scale(this->renderer, player_info->lerp_camera_fov * this->renderer->scale);
        rr_renderer_translate(this->renderer, -player_info->lerp_camera_x, -player_info->lerp_camera_y);
        for (EntityIdx p = 1; p < RR_MAX_ENTITY_COUNT; ++p)
        {
            if (rr_bitset_get_bit(this->simulation->arena_tracker, p) == 0)
                continue;
            rr_renderer_init_context_state(this->renderer, &state2);
            // intrusive op
            struct rr_component_arena *arena = rr_simulation_get_arena(this->simulation, p);
            rr_renderer_begin_path(this->renderer);
            rr_renderer_arc(this->renderer, 0, 0, arena->radius);
            rr_renderer_set_fill(this->renderer, 0xffeeeeee);
            rr_renderer_fill(this->renderer);
            rr_renderer_clip(this->renderer);

            rr_renderer_set_line_width(this->renderer, 1);
            rr_renderer_set_stroke(this->renderer, (uint32_t)(player_info->lerp_camera_fov * 51) << 24);

            float scale = player_info->lerp_camera_fov * this->renderer->scale;
            float leftX = player_info->lerp_camera_x - this->renderer->width / (2 * scale);
            float rightX = player_info->lerp_camera_x + this->renderer->width / (2 * scale);
            float topY = player_info->lerp_camera_y - this->renderer->height / (2 * scale);
            float bottomY = player_info->lerp_camera_y + this->renderer->height / (2 * scale);
            float newLeftX = ceilf(leftX / 50) * 50;
            float newTopY = ceilf(topY / 50) * 50;
            for (; newLeftX < rightX; newLeftX += 50)
            {
                rr_renderer_begin_path(this->renderer);
                rr_renderer_move_to(this->renderer, newLeftX, topY);
                rr_renderer_line_to(this->renderer, newLeftX, bottomY);
                rr_renderer_stroke(this->renderer);
            }
            for (; newTopY < bottomY; newTopY += 50)
            {
                rr_renderer_begin_path(this->renderer);
                rr_renderer_move_to(this->renderer, leftX, newTopY);
                rr_renderer_line_to(this->renderer, rightX, newTopY);
                rr_renderer_stroke(this->renderer);
            }
            rr_renderer_free_context_state(this->renderer, &state2);
            break; // only one arena
        }

        rr_simulation_for_each_entity(this->simulation, this, render_health_component);
        rr_simulation_for_each_entity(this->simulation, this, render_mob_component);
        rr_simulation_for_each_entity(this->simulation, this, render_petal_component);
        rr_simulation_for_each_entity(this->simulation, this, render_flower_component);
        rr_renderer_free_context_state(this->renderer, &state1);
    }
    this->socket->user_data = this;
    this->socket->on_event = rr_game_websocket_on_event_function;

#ifndef EMSCRIPTEN
    lws_service(this->socket->socket_context, -1);
#endif

    // ;
    if (rr_bitset_get_bit(this->input_data->keys_pressed_this_tick, 186))
        this->displaying_debug_information ^= 1;

    gettimeofday(&end, NULL);
    long time_elapsed = (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_usec - start.tv_usec);

    if (this->displaying_debug_information)
    {
        struct rr_renderer_context_state state;
        rr_renderer_init_context_state(this->renderer, &state);
        rr_renderer_scale(this->renderer, 5);
        static char debug_mspt[100];
        debug_mspt[sprintf(debug_mspt, "%f mspt", (float)time_elapsed / 1000.0f)] = 0;
        rr_renderer_fill_text(this->renderer, 0, 8, debug_mspt);
        rr_renderer_free_context_state(this->renderer, &state);
        // rr_renderer_stroke_text
    }

    memset(this->input_data->keys_pressed_this_tick, 0, 256 >> 3);
    memset(this->input_data->keys_released_this_tick, 0, 256 >> 3);
}