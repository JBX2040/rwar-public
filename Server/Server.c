#include <Server/Server.h>

#include <assert.h>
#include <math.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include <libwebsockets.h>

#include <Server/Client.h>
#include <Server/EntityAllocation.h>
#include <Server/Logs.h>
#include <Server/Simulation.h>
#include <Server/UpdateProtocol.h>
#include <Server/Waves.h>
#include <Shared/Api.h>
#include <Shared/Binary.h>
#include <Shared/Bitset.h>
#include <Shared/Component/Physical.h>
#include <Shared/Crypto.h>
#include <Shared/Rivet.h>
#include <Shared/Utilities.h>
#include <Shared/Vector.h>
#include <Shared/cJSON.h>
#include <Shared/pb.h>

#ifndef NDEBUG
#define MESSAGE_BUFFER_SIZE (32 * 1024 * 1024)
#else
#define MESSAGE_BUFFER_SIZE (1024 * 1024)
#endif

static uint8_t lws_message_data[MESSAGE_BUFFER_SIZE];
static uint8_t *outgoing_message = lws_message_data + LWS_PRE;

// static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void rr_api_on_open_result(char *bin, void *captures)
{
    struct rr_api_account *account = captures;
    struct rr_binary_encoder decoder;
    rr_binary_encoder_init(&decoder, (uint8_t *) bin);
    account->username = malloc(50 * (sizeof (char)));
    if (rr_binary_encoder_read_uint8(&decoder) != RR_API_SUCCESS)
    {
        puts("api serverside error");
        return;
    }
    rr_binary_encoder_read_nt_string(&decoder, account->username);
    if (strlen(account->username) < 30)
        return;
    account->xp = rr_binary_encoder_read_float64(&decoder);
    account->maximum_wave = rr_binary_encoder_read_varuint(&decoder);
    uint8_t id = rr_binary_encoder_read_uint8(&decoder);
    while (id)
    {
        uint32_t count = rr_binary_encoder_read_varuint(&decoder);
        uint8_t rarity = rr_binary_encoder_read_uint8(&decoder);
        account->petals[id][rarity] = count;
        id = rr_binary_encoder_read_uint8(&decoder);
    }
}

void rr_api_on_get_petals(char *bin, void *_client) {}

static void rr_server_client_create_player_info(struct rr_server *server, struct rr_server_client *client)
{
    puts("creating player info");
    struct rr_component_player_info *player_info = client->player_info = rr_simulation_add_player_info(
        &server->simulation,
        rr_simulation_alloc_entity(&server->simulation));
    player_info->squad = client->squad;
    rr_component_player_info_set_squad_pos(player_info, client->squad_pos);
    rr_component_player_info_set_slot_count(player_info, 10);
    player_info->level = client->level;
    #define min(a,b) (((a) < (b)) ? (a) : (b))
    rr_component_player_info_set_slot_count(client->player_info, min(10, 5 + client->level / RR_LEVELS_PER_EXTRA_SLOT));
    #undef min
    struct rr_component_arena *arena =
        rr_simulation_get_arena(&server->simulation, 1);
    struct rr_squad_member *member = rr_squad_get_client_slot(server, client);
    for (uint64_t i = 0; i < player_info->slot_count; ++i)
    {
        uint8_t id = member->loadout[i].id;
        uint8_t rarity = member->loadout[i].rarity;
        player_info->slots[i].id = id;
        player_info->slots[i].rarity = rarity;
        player_info->slots[i].count = RR_PETAL_DATA[id].count[rarity];

        id = member->loadout[i + 10].id;
        rarity = member->loadout[i + 10].rarity;
        player_info->secondary_slots[i].id = id;
        player_info->secondary_slots[i].rarity = rarity;
    }
}

void rr_server_client_free(struct rr_server_client *this)
{
    if (this->squad != 0)
    {
        rr_client_leave_squad(this->server, this);
    }
    
    if (this->player_info != NULL)
    {
        if (this->player_info->flower_id != RR_NULL_ENTITY)
            rr_simulation_request_entity_deletion(
                &this->server->simulation, this->player_info->flower_id);
        rr_simulation_request_entity_deletion(&this->server->simulation,
                                                this->player_info->parent_id);
    }
    puts("client disconnected");
}

void rr_server_client_encrypt_message(struct rr_server_client *this,
                                      uint8_t *start, uint64_t size)
{
    this->clientbound_encryption_key =
        rr_get_hash(this->clientbound_encryption_key);
    rr_encrypt(start, size, this->clientbound_encryption_key);
}

void rr_api_websocket_tick(struct rr_server *this)
{
    if (!this->api_ws_ready)
        return;
    /*
    struct rr_binary_encoder encoder;
    rr_binary_encoder_init(&encoder, outgoing_message);
    rr_binary_encoder_write_uint8(&encoder, 0); //api update packet
    lws_write(this->api_client, outgoing_message, encoder.at - encoder.start, LWS_WRITE_BINARY);
    */ 
}

void rr_server_client_write_message(struct rr_server_client *this,
                                    uint8_t *data, uint64_t size)
{
    lws_write(this->socket_handle, data, size, LWS_WRITE_BINARY);
}

void rr_server_client_broadcast_update(struct rr_server_client *this)
{
    struct rr_server *server = this->server;
    struct proto_bug encoder;
    proto_bug_init(&encoder, outgoing_message);
    proto_bug_write_uint8(&encoder, RR_CLIENTBOUND_UPDATE, "header");
    rr_simulation_write_binary(&server->simulation, &encoder,
                               this->player_info);
    rr_server_client_encrypt_message(this, encoder.start,
                                     encoder.current - encoder.start);
    rr_server_client_write_message(this, encoder.start,
                                   encoder.current - encoder.start);
}

int rr_server_client_tick(struct rr_server_client *this)
{
    if (this->squad == 0)
    {
        struct proto_bug encoder;
        proto_bug_init(&encoder, outgoing_message);
        proto_bug_write_uint8(&encoder, 234, "header");
        rr_server_client_encrypt_message(this, encoder.start,
                                         encoder.current - encoder.start);
        rr_server_client_write_message(this, encoder.start,
                                       encoder.current - encoder.start);
        return 123;
    }
    if (this->player_info != NULL)
    {
        if (rr_simulation_has_entity(&this->server->simulation,
                                     this->player_info->flower_id))
        {
            struct rr_component_physical *physical = rr_simulation_get_physical(
                &this->server->simulation, this->player_info->flower_id);
            rr_vector_set(&physical->acceleration, this->player_accel_x,
                          this->player_accel_y);
        }
        rr_server_client_broadcast_update(this);
        if (this->player_info->drops_this_tick_size > 0)
        {
            struct rr_binary_encoder encoder;
            rr_binary_encoder_init(&encoder, outgoing_message);
            rr_binary_encoder_write_uint8(&encoder, 2);
            rr_binary_encoder_write_nt_string(&encoder, this->rivet_account.uuid);
            rr_binary_encoder_write_uint8(&encoder, this->player_info->drops_this_tick_size);
            for (uint32_t i = 0; i < this->player_info->drops_this_tick_size; ++i)
            {
                uint8_t id = this->player_info->drops_this_tick[i].id;
                uint8_t rarity = this->player_info->drops_this_tick[i].rarity;
                ++this->inventory[id][rarity];
                rr_binary_encoder_write_uint8(&encoder, id);
                rr_binary_encoder_write_uint8(&encoder, rarity);
            }
            lws_write(this->server->api_client, encoder.start, encoder.at - encoder.start, LWS_WRITE_BINARY);
            this->player_info->drops_this_tick_size = 0;
            // this doesn't require a gameserver update. server synchronously updates drops
            // the only time the server needs to update is when the user crafts
        }
    }
    else
    {
        struct proto_bug encoder;
        proto_bug_init(&encoder, outgoing_message);
        proto_bug_write_uint8(&encoder, RR_CLIENTBOUND_SQUAD_UPDATE, "header");

        struct rr_squad *squad = &this->server->squads[this->squad + 1];
        for (uint32_t i = 0; i < RR_SQUAD_MEMBER_COUNT; ++i)
        {
            if (squad->members[i].in_use == 0)
            {
                proto_bug_write_uint8(
                &encoder,
                0,
                "bitbit");
                continue;
            }
            struct rr_squad_member *member = &squad->members[i];
            proto_bug_write_uint8(
                &encoder,
                1,
                "bitbit");
            proto_bug_write_uint8(&encoder, member->playing, "ready");
            proto_bug_write_string(&encoder, member->nickname,
                                   71, "nick");
            for (uint8_t j = 0; j < 20; ++j)
            {
                proto_bug_write_uint8(&encoder, member->loadout[j].id,
                                      "id");
                proto_bug_write_uint8(&encoder, member->loadout[j].rarity,
                                      "rar");
            }
        }
        proto_bug_write_uint8(&encoder, this->squad_pos, "sqpos");
        proto_bug_write_uint8(&encoder, squad->private, "private");
        proto_bug_write_uint8(&encoder, this->server->biome, "biome");
        char joined_code[16];
        joined_code[sprintf(joined_code, "%s-%s", this->server->server_alias, squad->squad_code)] = 0;
        proto_bug_write_string(&encoder, joined_code, 15, "squad code");
        rr_server_client_encrypt_message(this, encoder.start,
                                         encoder.current - encoder.start);
        rr_server_client_write_message(this, encoder.start,
                                       encoder.current - encoder.start);
    }
    return 345;
}

static void delete_entity_function(EntityIdx entity, void *_captures)
{
    if (rr_simulation_has_entity(_captures, entity))
        rr_simulation_request_entity_deletion(_captures, entity);
}

void rr_server_init(struct rr_server *this)
{
    //printf("server size: %lu\n", sizeof *this);
    memset(this, 0, sizeof *this);
#ifndef RIVET_BUILD
    this->biome = 1; 
#endif
    rr_static_data_init();
    rr_simulation_init(&this->simulation);
    this->simulation.biome = this->biome;
}

void rr_server_free(struct rr_server *this)
{
    lws_context_destroy(this->server);
}

static void rr_simulation_tick_entity_resetter_function(EntityIdx entity,
                                                        void *captures)
{
    struct rr_simulation *this = captures;
#define XX(COMPONENT, ID)                                                      \
    if (rr_simulation_has_##COMPONENT(this, entity))                           \
        rr_simulation_get_##COMPONENT(this, entity)->protocol_state = 0;
    RR_FOR_EACH_COMPONENT
#undef XX
}

void rr_server_tick(struct rr_server *this)
{
    if (!this->api_ws_ready)
        return;
    fputs("ministep1\n", stderr);
    rr_api_websocket_tick(this);
    fputs("ministep2\n", stderr);
    //rr_simulation_tick(&this->simulation);
    for (uint64_t i = 0; i < RR_MAX_CLIENT_COUNT; ++i)
    {
        if (rr_bitset_get(this->clients_in_use, i) && this->clients[i].verified && this->clients[i].received_first_packet)
        {
            struct rr_server_client *client = &this->clients[i];
            if (client->squad == 0 || client->squad > 10)
            {
                struct proto_bug encoder;
                proto_bug_init(&encoder, outgoing_message);
                proto_bug_write_uint8(&encoder, 234, "header");
                rr_server_client_encrypt_message(client, encoder.start,
                                                encoder.current - encoder.start);
                rr_server_client_write_message(client, encoder.start,
                                            encoder.current - encoder.start);
            }
            else if (client->player_info != NULL)
            {
                if (rr_simulation_has_entity(&this->simulation,
                                            client->player_info->flower_id))
                {
                    struct rr_component_physical *physical = rr_simulation_get_physical(
                        &this->simulation, client->player_info->flower_id);
                    rr_vector_set(&physical->acceleration, client->player_accel_x,
                                client->player_accel_y);
                }
                rr_server_client_broadcast_update(client);
                if (client->player_info->drops_this_tick_size > 0)
                {
                    struct rr_binary_encoder encoder;
                    rr_binary_encoder_init(&encoder, outgoing_message);
                    rr_binary_encoder_write_uint8(&encoder, 2);
                    rr_binary_encoder_write_nt_string(&encoder, client->rivet_account.uuid);
                    rr_binary_encoder_write_uint8(&encoder, client->player_info->drops_this_tick_size);
                    for (uint32_t i = 0; i < client->player_info->drops_this_tick_size; ++i)
                    {
                        uint8_t id = client->player_info->drops_this_tick[i].id;
                        uint8_t rarity = client->player_info->drops_this_tick[i].rarity;
                        ++client->inventory[id][rarity];
                        rr_binary_encoder_write_uint8(&encoder, id);
                        rr_binary_encoder_write_uint8(&encoder, rarity);
                    }
                    lws_write(this->api_client, encoder.start, encoder.at - encoder.start, LWS_WRITE_BINARY);
                    client->player_info->drops_this_tick_size = 0;
                    // this doesn't require a gameserver update. server synchronously updates drops
                    // the only time the server needs to update is when the user crafts
                }
            }
            else
            {
                struct proto_bug encoder;
                proto_bug_init(&encoder, outgoing_message);
                proto_bug_write_uint8(&encoder, RR_CLIENTBOUND_SQUAD_UPDATE, "header");

                struct rr_squad *squad = &this->squads[client->squad - 1];
                for (uint32_t i = 0; i < RR_SQUAD_MEMBER_COUNT; ++i)
                {
                    if (squad->members[i].in_use == 0)
                    {
                        proto_bug_write_uint8(
                        &encoder,
                        0,
                        "bitbit");
                        continue;
                    }
                    struct rr_squad_member *member = &squad->members[i];
                    proto_bug_write_uint8(
                        &encoder,
                        1,
                        "bitbit");
                    proto_bug_write_uint8(&encoder, member->playing, "ready");
                    for (uint8_t j = 0; j < 20; ++j)
                    {
                        proto_bug_write_uint8(&encoder, member->loadout[j].id,
                                            "id");
                        proto_bug_write_uint8(&encoder, member->loadout[j].rarity,
                                            "rar");
                    }
                }
                proto_bug_write_uint8(&encoder, client->squad_pos, "sqpos");
                proto_bug_write_uint8(&encoder, squad->private, "private");
                proto_bug_write_uint8(&encoder, this->biome, "biome");
                char joined_code[16];
                joined_code[sprintf(joined_code, "%s-%s", this->server_alias, squad->squad_code)] = 0;
                proto_bug_write_string(&encoder, joined_code, 15, "squad code");
                rr_server_client_encrypt_message(client, encoder.start,
                                                encoder.current - encoder.start);
                rr_server_client_write_message(client, encoder.start,
                                            encoder.current - encoder.start);
            }
        }
    }
    fputs("ministep3\n", stderr);
/*
    rr_simulation_for_each_entity(
        &this->simulation, &this->simulation,
        rr_simulation_tick_entity_resetter_function);
*/
}

static int handle_lws_event(struct rr_server *this, struct lws *ws,
                            enum lws_callback_reasons reason, uint8_t *packet,
                            size_t size)
{
    switch (reason)
    {
    case LWS_CALLBACK_ESTABLISHED:
    {
        if (!this->api_ws_ready)
        {
            lws_close_reason(ws, LWS_CLOSE_STATUS_GOINGAWAY,
                         (uint8_t *)"api ws not ready",
                         sizeof "api ws not ready" - 1);
            return -1;
        }
        for (uint64_t i = 0; i < RR_MAX_CLIENT_COUNT; i++)
            if (!rr_bitset_get_bit(this->clients_in_use, i))
            {
                rr_bitset_set(this->clients_in_use, i);
                rr_server_client_init(this->clients + i);
                this->clients[i].server = this;
                this->clients[i].file_descriptor = lws_get_socket_fd(ws);
                this->clients[i].socket_handle = ws;
                // send encryption key
                struct proto_bug encryption_key_encoder;
                proto_bug_init(&encryption_key_encoder, outgoing_message);
                proto_bug_write_uint64(&encryption_key_encoder,
                                       this->clients[i].requested_verification,
                                       "verification");
                proto_bug_write_uint32(&encryption_key_encoder, rr_get_rand(),
                                       "useless bytes");
                proto_bug_write_uint64(
                    &encryption_key_encoder,
                    this->clients[i].clientbound_encryption_key,
                    "c encryption key");
                proto_bug_write_uint64(
                    &encryption_key_encoder,
                    this->clients[i].serverbound_encryption_key,
                    "s encryption key");
                rr_encrypt(outgoing_message, 1024, 21094093777837637ull);
                rr_encrypt(outgoing_message, 8, 1);
                rr_encrypt(outgoing_message, 1024, 59731158950470853ull);
                rr_encrypt(outgoing_message, 1024, 64709235936361169ull);
                rr_encrypt(outgoing_message, 1024, 59013169977270713ull);
                rr_server_client_write_message(this->clients + i,
                                               outgoing_message, 1024);
                return 0;
            }

        lws_close_reason(ws, LWS_CLOSE_STATUS_GOINGAWAY,
                         (uint8_t *)"too many active clients",
                         sizeof "to many active clients" - 1);
        return -1;
    }
    case LWS_CALLBACK_CLOSED:
    {
        int file_descriptor = lws_get_socket_fd(ws);
        for (uint64_t i = 0; i < RR_MAX_CLIENT_COUNT; i++)
            if (rr_bitset_get(this->clients_in_use, i))
            {
                if (this->clients[i].file_descriptor == file_descriptor)
                {
                    rr_bitset_unset(this->clients_in_use, i);
#ifdef RIVET_BUILD
                    rr_rivet_players_disconnected(
                        getenv("RIVET_TOKEN"),
                        this->clients[i].rivet_account.token);
#endif
                    struct rr_binary_encoder encoder;
                    rr_binary_encoder_init(&encoder, outgoing_message);
                    rr_binary_encoder_write_uint8(&encoder, 1);
                    rr_binary_encoder_write_nt_string(&encoder, this->clients[i].rivet_account.uuid);
                    rr_binary_encoder_write_uint8(&encoder, i);
                    lws_write(this->api_client, encoder.start, encoder.at - encoder.start, LWS_WRITE_BINARY);
                    rr_server_client_free(this->clients + i);
                    char log[100] = {"ip: `"};
                    strcat(log, this->clients[i].ip_address);
                    strcat(log, "`");
                    // rr_discord_webhook_log("player status", "client
                    // disconnected", log, 0xff4444);
                    return 0;
                }
            }
        puts("client joined but instakicked");
        // RR_UNREACHABLE("couldn't remove client");
        break;
    }
    case LWS_CALLBACK_SERVER_WRITEABLE:
    {
        break;
    }
    case LWS_CALLBACK_RECEIVE:
    {
        struct rr_server_client *client = NULL;
        uint64_t i = 0;
        uint32_t first = 1;
        for (; i < RR_MAX_CLIENT_COUNT; i++)
        {
            if (!rr_bitset_get(this->clients_in_use, i))
                continue;
            if (this->clients[i].file_descriptor == lws_get_socket_fd(ws))
            {
                client = &this->clients[i];
                break;
            }
            first = 0;
        }
        rr_decrypt(packet, size, client->serverbound_encryption_key);
        client->serverbound_encryption_key =
            rr_get_hash(rr_get_hash(client->serverbound_encryption_key));
        struct proto_bug encoder;
        proto_bug_init(&encoder, packet);
        proto_bug_set_bound(&encoder, packet + size);
        if (!client->received_first_packet)
        {
            client->received_first_packet = 1;

            proto_bug_read_uint64(&encoder, "useless bytes");
            uint64_t received_verification =
            proto_bug_read_uint64(&encoder, "verification");
            if (received_verification != client->requested_verification)
            {
                printf("%lu %lu\n", client->requested_verification, received_verification);
                fputs("invalid verification\n", stderr);
                lws_close_reason(ws, LWS_CLOSE_STATUS_GOINGAWAY,
                                 (uint8_t *)"invalid v", sizeof "invalid v"-1);
                return -1;
            }

            memset(&client->rivet_account, 0,
                   sizeof(struct rr_rivet_account));
            // Read rivet token
            proto_bug_read_string(&encoder,
                                  client->rivet_account.token,
                                  300, "rivet token");
            // Read uuid
            proto_bug_read_string(&encoder, client->rivet_account.uuid,
                                  100, "rivet uuid");
            //puts(client->rivet_account.uuid);
#ifdef RIVET_BUILD
            for (uint32_t j = 0; j < RR_MAX_CLIENT_COUNT; ++j)
            {
                if (!rr_bitset_get(this->clients_in_use, j))
                    continue;
                if (i == j)
                    continue;
                if (strcmp(client->rivet_account.uuid, this->clients[j].rivet_account.uuid) == 0)
                {
                    fputs("skid multibox\n", stderr);
                    lws_close_reason(ws, LWS_CLOSE_STATUS_GOINGAWAY,
                                        (uint8_t *)"skid multibox",
                                        sizeof "skid multibox");
                    return -1;
                }
            }
#endif
            if (proto_bug_read_varuint(&encoder, "dev flag") == 494538643243)
                client->dev = 1;

#ifdef RIVET_BUILD
            printf("client connecting with token: %s\n",
                    client->rivet_account.token);
            if (!rr_rivet_players_connected(
                    getenv("RIVET_TOKEN"),
                    client->rivet_account.token))
            {
                fputs("rivet error\n", stderr);
                lws_close_reason(ws, LWS_CLOSE_STATUS_GOINGAWAY,
                                (uint8_t *)"rivet error",
                                sizeof "rivet error");
                return -1;
            }
#endif
            //struct rr_api_account account = {0};
            //rr_api_on_open(client->rivet_account.uuid, &account);

            //client->max_wave = account.maximum_wave;

            printf("socket %s verified\n", client->rivet_account.uuid);
            struct rr_binary_encoder encoder;
            rr_binary_encoder_init(&encoder, outgoing_message);
            rr_binary_encoder_write_uint8(&encoder, 0);
            rr_binary_encoder_write_nt_string(&encoder, client->rivet_account.uuid);
            rr_binary_encoder_write_uint8(&encoder, i);
            lws_write(this->api_client, encoder.start, encoder.at - encoder.start, LWS_WRITE_BINARY);
            //rr_client_join_squad(this, client, 0);
            return 0;
        }
        if (!client->verified)
            break;
        switch (proto_bug_read_uint8(&encoder, "header"))
        {
        case RR_SERVERBOUND_INPUT:
        {
            if (client->player_info == NULL)
                break;
            if (client->player_info->flower_id == RR_NULL_ENTITY)
                break;
            uint8_t movementFlags =
                proto_bug_read_uint8(&encoder, "movement kb flags");
            float x = 0;
            float y = 0;

            if ((movementFlags & 64) == 0)
            {
                y -= (movementFlags & 1) >> 0;
                x -= (movementFlags & 2) >> 1;
                y += (movementFlags & 4) >> 2;
                x += (movementFlags & 8) >> 3;
                if (x || y)
                {
                    float mag_1 = RR_PLAYER_SPEED / sqrtf(x * x + y * y);
                    x *= mag_1;
                    y *= mag_1;
                }
            }
            else
            {
                x = proto_bug_read_float32(&encoder, "mouse x");
                y = proto_bug_read_float32(&encoder, "mouse y");
                if ((x != 0 || y != 0) && fabsf(x) < 10000 && fabsf(y) < 10000)
                {
                    float mag_1 = sqrtf(x * x + y * y);
                    float scale = RR_PLAYER_SPEED * rr_fclamp((mag_1 - 25) / 50, 0, 1);
                    x *= scale / mag_1;
                    y *= scale / mag_1;
                }
            }
            if ((x != 0 || y != 0) && fabsf(x) < 10000 && fabsf(y) < 10000)
            {
                client->player_accel_x = x;
                client->player_accel_y = y;
            }
            else
            {
                client->player_accel_x = 0;
                client->player_accel_y = 0;
            }

            client->player_info->input = (movementFlags >> 4) & 3;
            break;
        }
        case RR_SERVERBOUND_PETAL_SWITCH:
        {
            if (client->player_info->flower_id == RR_NULL_ENTITY)
                return 0;
            uint8_t pos = proto_bug_read_uint8(&encoder, "petal switch");
            while (pos != 0 && pos <= 10 &&
                   encoder.current - encoder.start <= size)
            {
                rr_component_player_info_petal_swap(client->player_info,
                                                    &this->simulation, pos - 1);
                pos = proto_bug_read_uint8(&encoder, "petal switch");
            }
            break;
        }
        case RR_SERVERBOUND_SQUAD_JOIN:
        {
            uint8_t type = proto_bug_read_uint8(&encoder, "join type");
            fprintf(stderr, "client %lu attempting a squad create of type %d\n", i, type);
            if (type > 2)
                break;
            rr_client_leave_squad(this, client);
            uint8_t squad = RR_ERROR_CODE_INVALID_SQUAD;
            if (type == 2)
                squad = rr_client_create_squad(this, client);
            else if (type == 1)
            {
                char link[16];
                proto_bug_read_string(&encoder, link, 6, "connect link");
                squad = rr_client_join_squad_with_code(this, link);
            }
            else if (type == 0)
                squad = rr_client_find_squad(this, client);
            if (squad == RR_ERROR_CODE_INVALID_SQUAD || squad == RR_ERROR_CODE_FULL_SQUAD)
            {
                struct proto_bug failure;
                proto_bug_init(&failure, outgoing_message);
                proto_bug_write_uint8(&failure, RR_CLIENTBOUND_SQUAD_FAIL, "header");
                proto_bug_write_uint8(&failure, 0, "fail type");
                rr_server_client_encrypt_message(client, failure.start,
                                failure.current - failure.start);
                rr_server_client_write_message(client, failure.start,
                            failure.current - failure.start);
                client->squad = 0;
                break;
            }
            rr_client_join_squad(this, client, squad);
            fprintf(stderr, "client joining squad %d\n", squad);
            break;
        }
        case RR_SERVERBOUND_SQUAD_READY:
        {
            if (client->squad != 0)
            {
                if (rr_squad_get_client_slot(this, client)->playing == 0)
                {
                    rr_squad_get_client_slot(this, client)->playing = 1;
                    rr_server_client_create_player_info(this, client);
                    rr_server_client_create_flower(client);
                }
                else
                {
                    rr_squad_get_client_slot(this, client)->playing = 0;
                    if (client->player_info != NULL)
                    {
                        if (client->player_info->flower_id != RR_NULL_ENTITY)
                            rr_simulation_request_entity_deletion(
                                &this->simulation, client->player_info->flower_id);
                        rr_simulation_request_entity_deletion(&this->simulation,
                                                             client->player_info->parent_id);
                        client->player_info = NULL;
                    }
                }
            }
            break;
        }
        case RR_SERVERBOUND_LOADOUT_UPDATE:
        {
            uint8_t loadout_count =
                proto_bug_read_uint8(&encoder, "loadout count");

            if (loadout_count > 10)
                break;
            struct rr_squad_member *member = rr_squad_get_client_slot(this, client);
            if (member == NULL)
                break;
            uint32_t temp_inv[rr_petal_id_max][rr_rarity_id_max];

            memcpy(temp_inv, client->inventory, sizeof client->inventory);
            for (uint8_t i = 0; i < loadout_count; i++)
            {
                uint8_t id = proto_bug_read_uint8(&encoder, "id");
                uint8_t rarity = proto_bug_read_uint8(&encoder, "rarity");
                if (id >= rr_petal_id_max)
                    break;
                if (rarity >= rr_rarity_id_max)
                    break;
                member->loadout[i].rarity = rarity;
                member->loadout[i].id = id;
                if (id && temp_inv[id][rarity]-- == 0)
                {
                    memset(member->loadout, 0, sizeof member->loadout);
                    break;
                }
                id = proto_bug_read_uint8(&encoder, "id");
                rarity = proto_bug_read_uint8(&encoder, "rarity");
                if (id >= rr_petal_id_max)
                    break;
                if (rarity >= rr_rarity_id_max)
                    break;
                member->loadout[i + 10].rarity = rarity;
                member->loadout[i + 10].id = id;
                if (id && temp_inv[id][rarity]-- == 0)
                {
                    memset(member->loadout, 0, sizeof member->loadout);
                    break;
                }
            }

            break;
        }
        case RR_SERVERBOUND_NICKNAME_UPDATE:
        {
            proto_bug_read_string(&encoder, client->client_nickname,
                                  16 + 1, "nick");
            break;
        }
        case RR_SERVERBOUND_PRIVATE_UPDATE:
        {
            if (client->squad_pos != 0 && first)
                this->squads[client->squad - 1].private = 0;
            break;
        }
        default:
            break;
        }
        return 0;
    }
    default:
        return 0;
    }

    return 0;
}

static int api_lws_callback(struct lws *ws, enum lws_callback_reasons reason,
                        void *user, void *packet, size_t size)
{
    struct rr_server *this =
        (struct rr_server *)lws_context_user(lws_get_context(ws));
    //printf("packet rec + connect %llu %p\n", reason, this);
    switch (reason)
    {
    case LWS_CALLBACK_CLIENT_ESTABLISHED:
    {
        this->api_ws_ready = 1;
        char *lobby_id = 
        #ifdef RIVET_BUILD
        getenv("RIVET_LOBBY_ID");
        #else
        "localhost";
        #endif
        struct rr_binary_encoder encoder;
        rr_binary_encoder_init(&encoder, outgoing_message);
        rr_binary_encoder_write_uint8(&encoder, 101);
        rr_binary_encoder_write_nt_string(&encoder, lobby_id);
        lws_write(this->api_client, encoder.start, encoder.at - encoder.start, LWS_WRITE_BINARY);
    }
        break;
    case LWS_CALLBACK_CLIENT_RECEIVE:
    {
        //parse incoming client data
        struct rr_binary_encoder decoder;
        rr_binary_encoder_init(&decoder, packet);
        if (rr_binary_encoder_read_uint8(&decoder) != RR_API_SUCCESS)
            break;
        switch (rr_binary_encoder_read_uint8(&decoder))
        {
            case 0:
            {
                rr_binary_encoder_read_nt_string(&decoder, this->server_alias);
                //printf("server link is %s\n", this->server_alias);
                break;
            }
            case 1:
            {
                uint8_t pos = rr_binary_encoder_read_uint8(&decoder);
                char uuid[100];
                rr_binary_encoder_read_nt_string(&decoder, uuid);
                struct rr_server_client *client = &this->clients[pos];
                if (strcmp(uuid, client->rivet_account.uuid))
                    break; // fake client
                double xp = rr_binary_encoder_read_float64(&decoder);
                uint32_t next_level = 2;
                while (xp >= xp_to_reach_level(next_level))
                {
                    xp -= xp_to_reach_level(next_level);
                    ++next_level;
                }
                #define min(a,b) (((a) < (b)) ? (a) : (b))
                client->level = min(next_level - 1, 150);
                #undef min
                printf("reading account %s, client level is %d %f\n", uuid, client->level, xp);
                uint8_t id = rr_binary_encoder_read_uint8(&decoder);
                while (id)
                {
                    uint32_t count = rr_binary_encoder_read_varuint(&decoder);
                    uint8_t rarity = rr_binary_encoder_read_uint8(&decoder);
                    client->inventory[id][rarity] = count;
                    id = rr_binary_encoder_read_uint8(&decoder);
                }
                client->verified = 1;
                break;
            }
            case 100:
            {
                struct rr_binary_encoder encoder;
                rr_binary_encoder_init(&encoder, outgoing_message);
                rr_binary_encoder_write_uint8(&encoder, 100);
                lws_write(this->api_client, encoder.start, encoder.at - encoder.start, LWS_WRITE_BINARY);
                break;
            }
            default:
                break;
        }
        break;
    }
    case LWS_CALLBACK_CLIENT_CLOSED:
        //uh oh
        fprintf(stderr, "api ws disconnected\n");
        abort();
        break;
    case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
        fprintf(stderr, "api ws refused to connect\n");
        abort();
        break;
    default:
        return 0;
    }
    return 0;
}

static int lws_callback(struct lws *ws, enum lws_callback_reasons reason,
                        void *user, void *packet, size_t size)
{
    switch (reason)
    {
    case LWS_CALLBACK_ESTABLISHED:
    case LWS_CALLBACK_SERVER_WRITEABLE:
    case LWS_CALLBACK_RECEIVE:
    case LWS_CALLBACK_CLOSED:
        break;
    default:
        return 0;
    }
    // assert(pthread_mutex_lock(&mutex) == 0);

    struct rr_server *this =
        (struct rr_server *)lws_context_user(lws_get_context(ws));
    int close = handle_lws_event(this, ws, reason, packet, size);
    if (close)
        return close;
    // assert(pthread_mutex_unlock(&mutex) == 0);
    return 0;
}

void *thread_func(void *arg)
{
    struct rr_server *this = (struct rr_server *)arg;
    while (1)
    {
        lws_service(this->server, 0);
    }
    return 0;
}

static void lws_log(int level, char const *log) { printf("%d %s", level, log); }

void rr_server_run(struct rr_server *this)
{
    {
        struct lws_context_creation_info info = {0};

        info.protocols = (struct lws_protocols[]){
            {"g", lws_callback, sizeof(uint8_t), MESSAGE_BUFFER_SIZE, 0, NULL, 0},
            {0}};

        info.port = 1234;
        info.user = this;
        info.pt_serv_buf_size = MESSAGE_BUFFER_SIZE;
        lws_set_log_level(0, lws_log);

        this->server = lws_create_context(&info);
        assert(this->server);
    }

    {
        struct lws_context_creation_info info = {0};

        info.protocols = (struct lws_protocols[]){
            {"g", api_lws_callback, sizeof(uint8_t), MESSAGE_BUFFER_SIZE, 0, NULL, 0},
            {0}};

        info.port = CONTEXT_PORT_NO_LISTEN;
        info.user = this;
        info.pt_serv_buf_size = 16 * 1024;
        this->api_client_context = lws_create_context(&info);
        struct lws_client_connect_info connection_info;
        memset(&connection_info, 0, sizeof connection_info);
        connection_info.context = this->api_client_context;
        connection_info.address = 
        #ifdef RIVET_BUILD
            "45.79.197.197";
        #else
            "localhost";
        #endif
        connection_info.port = 55554;
        connection_info.path = "api/" RR_API_SECRET;
        connection_info.host = lws_canonical_hostname(this->api_client_context);
        connection_info.origin = RR_BASE_API_URL;
        connection_info.protocol = "h";
        this->api_client = lws_client_connect_via_info(&connection_info);
    }

    while (1)
    {
        struct timeval start;
        struct timeval end;

        gettimeofday(&start,
                     NULL); // gettimeofday actually starts from unix
                            // timestamp 0 (goofy)
        lws_service(this->server, -1);
        lws_service(this->api_client_context, -1);
        fputs("step2\n", stderr);
        if (!this->api_ws_ready)
            continue;
    fputs("ministep1\n", stderr);
    rr_api_websocket_tick(this);
    fputs("ministep2\n", stderr);
    //rr_simulation_tick(&this->simulation);
    for (uint64_t i = 0; i < RR_MAX_CLIENT_COUNT; ++i)
    {
        if (rr_bitset_get(this->clients_in_use, i) && this->clients[i].verified && this->clients[i].received_first_packet)
        {
            struct rr_server_client *client = &this->clients[i];
            if (client->squad == 0 || client->squad > 10)
            {
                struct proto_bug encoder;
                proto_bug_init(&encoder, outgoing_message);
                proto_bug_write_uint8(&encoder, 234, "header");
                rr_server_client_encrypt_message(client, encoder.start,
                                                encoder.current - encoder.start);
                rr_server_client_write_message(client, encoder.start,
                                            encoder.current - encoder.start);
            }
            else if (client->player_info != NULL)
            {
                if (rr_simulation_has_entity(&this->simulation,
                                            client->player_info->flower_id))
                {
                    struct rr_component_physical *physical = rr_simulation_get_physical(
                        &this->simulation, client->player_info->flower_id);
                    rr_vector_set(&physical->acceleration, client->player_accel_x,
                                client->player_accel_y);
                }
                rr_server_client_broadcast_update(client);
                if (client->player_info->drops_this_tick_size > 0)
                {
                    struct rr_binary_encoder encoder;
                    rr_binary_encoder_init(&encoder, outgoing_message);
                    rr_binary_encoder_write_uint8(&encoder, 2);
                    rr_binary_encoder_write_nt_string(&encoder, client->rivet_account.uuid);
                    rr_binary_encoder_write_uint8(&encoder, client->player_info->drops_this_tick_size);
                    for (uint32_t i = 0; i < client->player_info->drops_this_tick_size; ++i)
                    {
                        uint8_t id = client->player_info->drops_this_tick[i].id;
                        uint8_t rarity = client->player_info->drops_this_tick[i].rarity;
                        ++client->inventory[id][rarity];
                        rr_binary_encoder_write_uint8(&encoder, id);
                        rr_binary_encoder_write_uint8(&encoder, rarity);
                    }
                    lws_write(this->api_client, encoder.start, encoder.at - encoder.start, LWS_WRITE_BINARY);
                    client->player_info->drops_this_tick_size = 0;
                    // this doesn't require a gameserver update. server synchronously updates drops
                    // the only time the server needs to update is when the user crafts
                }
            }
            else
            {
                struct proto_bug encoder;
                proto_bug_init(&encoder, outgoing_message);
                proto_bug_write_uint8(&encoder, RR_CLIENTBOUND_SQUAD_UPDATE, "header");

                struct rr_squad *squad = &this->squads[client->squad - 1];
                for (uint32_t i = 0; i < RR_SQUAD_MEMBER_COUNT; ++i)
                {
                    if (squad->members[i].in_use == 0)
                    {
                        proto_bug_write_uint8(
                        &encoder,
                        0,
                        "bitbit");
                        continue;
                    }
                    struct rr_squad_member *member = &squad->members[i];
                    proto_bug_write_uint8(
                        &encoder,
                        1,
                        "bitbit");
                    proto_bug_write_uint8(&encoder, member->playing, "ready");
                    for (uint8_t j = 0; j < 20; ++j)
                    {
                        proto_bug_write_uint8(&encoder, member->loadout[j].id,
                                            "id");
                        proto_bug_write_uint8(&encoder, member->loadout[j].rarity,
                                            "rar");
                    }
                }
                proto_bug_write_uint8(&encoder, client->squad_pos, "sqpos");
                proto_bug_write_uint8(&encoder, squad->private, "private");
                proto_bug_write_uint8(&encoder, this->biome, "biome");
                char joined_code[16];
                joined_code[sprintf(joined_code, "%s-%s", this->server_alias, squad->squad_code)] = 0;
                proto_bug_write_string(&encoder, joined_code, 15, "squad code");
                rr_server_client_encrypt_message(client, encoder.start,
                                                encoder.current - encoder.start);
                rr_server_client_write_message(client, encoder.start,
                                            encoder.current - encoder.start);
            }
        }
    }
    fputs("ministep3\n", stderr);
        //rr_server_tick(this);
        fputs("step3\n", stderr);
        gettimeofday(&end, NULL);

        uint64_t elapsed_time = (end.tv_sec - start.tv_sec) * 1000000 +
                                (end.tv_usec - start.tv_usec);
        //if (elapsed_time > 25000)
            //fprintf(stderr, "tick took %lu microseconds\n", elapsed_time);
        int64_t to_sleep = 40000 - elapsed_time;
        if (to_sleep > 0)
            usleep(to_sleep);
    }
}
