
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <libgen.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "client.h"
#include "config.h"
#include "cube.h"
#include "db.h"
#include "door.h"
#include "fence.h"
#include "item.h"
#include "map.h"
#include "matrix.h"
#include "noise.h"
#include "pg.h"
#include "pg_joystick.h"
#include "pw.h"
#include "pwlua.h"
#include "pwlua_standalone.h"
#include "pwlua_worldgen.h"
#include "ring.h"
#include "sign.h"
#include "tinycthread.h"
#include "ui.h"
#include "util.h"
#include "world.h"
#include "x11_event_handler.h"

#define MAX_CHUNKS 8192
#define MAX_CLIENTS 128
#define WORKERS 1
#define MAX_NAME_LENGTH 32

#define MAX_HISTORY_SIZE 20
#define CHAT_HISTORY 0
#define COMMAND_HISTORY 1
#define SIGN_HISTORY 2
#define LUA_HISTORY 3
#define NUM_HISTORIES 4
#define NOT_IN_HISTORY -1

#define MODE_OFFLINE 0
#define MODE_ONLINE 1

#define WORKER_IDLE 0
#define WORKER_BUSY 1
#define WORKER_DONE 2

#define UNASSIGNED -1

#define DEADZONE 0.0

#define CROUCH_JUMP 0
#define REMOVE_ADD 1
#define CYCLE_DOWN_UP 2
#define FLY_PICK 3
#define ZOOM_ORTHO 4
#define SHOULDER_BUTTON_MODE_COUNT 5
const char *shoulder_button_modes[SHOULDER_BUTTON_MODE_COUNT] = {
    "Crouch/Jump",
    "Remove/Add",
    "Previous/Next",
    "Fly/Pick",
    "Zoom/Ortho"
};

const float RED[4] = {1.0, 0.0, 0.0, 1.0};
const float GREEN[4] = {0.0, 1.0, 0.0, 1.0};
const float BLACK[4] = {0.0, 0.0, 0.0, 1.0};

float hud_text_background[4] = {0.4, 0.4, 0.4, 0.4};
float hud_text_color[4] = {0.85, 0.85, 0.85, 1.0};

static int terminate;

typedef struct {
    Map map;
    Map extra;
    Map lights;
    Map shape;
    SignList signs;
    Map transform;
    DoorMap doors;
    int p;
    int q;
    int faces;
    int sign_faces;
    int dirty;
    int dirty_signs;
    int miny;
    int maxy;
    GLuint buffer;
    GLuint sign_buffer;
} Chunk;

typedef struct {
    int p;
    int q;
    int load;
    Map *block_maps[3][3];
    Map *extra_maps[3][3];
    Map *light_maps[3][3];
    Map *shape_maps[3][3];
    Map *transform_maps[3][3];
    DoorMap *door_maps[3][3];
    SignList signs;
    int miny;
    int maxy;
    int faces;
    void *data;
} WorkerItem;

typedef struct {
    int index;
    int state;
    thrd_t thrd;
    mtx_t mtx;
    cnd_t cnd;
    WorkerItem item;
    int exit_requested;
} Worker;

mtx_t edit_ring_mtx;

typedef struct {
    int x;
    int y;
    int z;
    int w;
} Block;

typedef struct {
    float x;
    float y;
    float z;
    float rx;
    float ry;
    float t;
} State;

typedef struct Player {
    char name[MAX_NAME_LENGTH];
    int id;  // 1...MAX_LOCAL_PLAYERS
    State state;
    State state1;
    State state2;
    GLuint buffer;
    int texture_index;
    int is_active;
} Player;

typedef struct {
    int id;
    Player players[MAX_LOCAL_PLAYERS];
} Client;

typedef struct {
    char lines[MAX_HISTORY_SIZE][MAX_TEXT_LENGTH];
    int size;
    int end;
    size_t line_start;
} TextLineHistory;

typedef struct {
    int x;
    int y;
    int z;
    int texture;
    int extra;
    int light;
    int shape;
    int transform;
    SignList signs;
    int has_sign;
} UndoBlock;

typedef struct {
    Player *player;
    int item_index;
    int flying;
    float dy;
    char messages[MAX_MESSAGES][MAX_TEXT_LENGTH];
    int message_index;

    int typing;
    char typing_buffer[MAX_TEXT_LENGTH];
    TextLineHistory typing_history[NUM_HISTORIES];
    int history_position;
    size_t text_cursor;
    size_t typing_start;

    LuaThreadState* lua_shell;

    int mouse_x;
    int mouse_y;
    Menu menu;
    int menu_id_resume;
    int menu_id_options;
    int menu_id_new;
    int menu_id_load;
    int menu_id_exit;
    Menu menu_options;
    int menu_id_script;
    int menu_id_crosshairs;
    int menu_id_fullscreen;
    int menu_id_verbose;
    int menu_id_wireframe;
    int menu_id_worldgen;
    int menu_id_options_resume;
    Menu menu_new;
    int menu_id_new_game_name;
    int menu_id_new_ok;
    int menu_id_new_cancel;
    Menu menu_load;
    int menu_id_load_cancel;
    Menu menu_item_in_hand;
    int menu_id_item_in_hand_cancel;
    Menu menu_block_edit;
    int menu_id_block_edit_resume;
    int menu_id_texture;
    int menu_id_sign_text;
    int menu_id_light;
    int menu_id_control_bit;
    int menu_id_open_bit;
    int menu_id_shape;
    int menu_id_transform;
    int edit_x, edit_y, edit_z, edit_face;
    Menu menu_texture;
    int menu_id_texture_cancel;
    Menu menu_shape;
    int menu_id_shape_cancel;
    Menu menu_transform;
    int menu_id_transform_cancel;
    Menu menu_script;
    int menu_id_script_cancel;
    int menu_id_script_run;
    Menu menu_script_run;
    char menu_script_run_dir[MAX_DIR_LENGTH];
    int menu_id_script_run_cancel;
    Menu menu_worldgen;
    int menu_id_worldgen_cancel;
    int menu_id_worldgen_select;
    Menu menu_worldgen_select;
    char menu_worldgen_dir[MAX_DIR_LENGTH];
    int menu_id_worldgen_select_cancel;
    Menu *active_menu;

    int view_x;
    int view_y;
    int view_width;
    int view_height;

    int forward_is_pressed;
    int back_is_pressed;
    int left_is_pressed;
    int right_is_pressed;
    int jump_is_pressed;
    int crouch_is_pressed;
    int view_left_is_pressed;
    int view_right_is_pressed;
    int view_up_is_pressed;
    int view_down_is_pressed;
    int ortho_is_pressed;
    int zoom_is_pressed;
    float view_speed_left_right;
    float view_speed_up_down;
    float movement_speed_left_right;
    float movement_speed_forward_back;
    int shoulder_button_mode;

    Block block0;
    Block block1;
    Block copy0;
    Block copy1;

    UndoBlock undo_block;
    int has_undo_block;

    int observe1;
    int observe1_client_id;
    int observe2;
    int observe2_client_id;

    int mouse_id;
    int keyboard_id;
    int joystick_id;
} LocalPlayer;

typedef struct {
    Worker workers[WORKERS];
    Chunk chunks[MAX_CHUNKS];
    int chunk_count;
    int create_radius;
    int render_radius;
    int delete_radius;
    int sign_radius;
    Client clients[MAX_CLIENTS];
    LocalPlayer local_players[MAX_LOCAL_PLAYERS];
    int client_count;
    int width;
    int height;
    float scale;
    int ortho;
    float fov;
    int suppress_char;
    int mode;
    int mode_changed;
    int render_option_changed;
    char db_path[MAX_PATH_LENGTH];
    int day_length;
    int time_changed;
    int auto_add_players_on_new_devices;
    int gl_float_type;
    size_t float_size;
    lua_State *lua_worldgen;
    int use_lua_worldgen;
    Ring edit_ring;
} Model;

static Model model;
static Model *g = &model;

void set_players_view_size(int w, int h);
Client *find_client(int id);
void set_view_radius(int requested_size, int delete_request);
LocalPlayer* player_for_keyboard(int keyboard_id);
LocalPlayer* player_for_mouse(int mouse_id);
LocalPlayer* player_for_joystick(int joystick_id);
void _set_extra(int p, int q, int x, int y, int z, int w, int dirty);
void _set_shape(int p, int q, int x, int y, int z, int w, int dirty);
int get_shape(int x, int y, int z);
void _set_transform(int p, int q, int x, int y, int z, int w, int dirty);
void cancel_player_inputs(LocalPlayer *p);
void open_menu(LocalPlayer *local, Menu *menu);
void close_menu(LocalPlayer *local);
void handle_menu_event(LocalPlayer *local, Menu *menu, int event);
void populate_texture_menu(Menu *menu);
void populate_shape_menu(Menu *menu);
void populate_transform_menu(Menu *menu);
void populate_block_edit_menu(LocalPlayer *local, int w, char *sign_text,
    int light, int extra, int shape, int transform);

// returns 1 if limit applied, 0 if no limit applied
int limit_player_count_to_fit_gpu_mem(void)
{
    if (!config->no_limiters &&
        pg_get_gpu_mem_size() < 64 && config->players > 2) {
        printf("More GPU memory needed for more players.\n");
        config->players = 2;
        return 1;
    }
    return 0;
}

void set_player_count(Client *client, int count)
{
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        Player *player = client->players + i;
        if (i < count) {
            player->is_active = 1;
        } else {
            player->is_active = 0;
        }
    }
}

int get_first_active_player(Client *client) {
    int first_active_player = 0;
    for (int i=0; i<MAX_LOCAL_PLAYERS-1; i++) {
        Player *player = &client->players[i];
        if (player->is_active) {
            first_active_player = i;
            break;
        }
    }
    return first_active_player;
}

int get_next_local_player(Client *client, int start)
{
    int next = get_first_active_player(client);

    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        Player *player = &client->players[i];
        if (i > start && player->is_active) {
            next = i;
            break;
        }
    }
    return next;
}

void get_next_player(int *player_index, int *client_id)
{
    Client *client = find_client(*client_id);
    if (!client) {
        client = g->clients;
        *client_id = client->id;
        *player_index = 1;
    }
    int next = get_next_local_player(client, *player_index);
    if (next <= *player_index) {
        *client_id = (*client_id + 1) % (g->client_count + 1);
        if (*client_id == 0) {
            *client_id = 1;
        }
        client = find_client(*client_id);
        if (!client) {
            client = g->clients;
            *client_id = client->id;
        }
        next = get_first_active_player(client);
    }
    *player_index = next;
}

int chunked(float x) {
    return floorf(roundf(x) / CHUNK_SIZE);
}

float time_of_day(void) {
    if (g->day_length <= 0) {
        return 0.5;
    }
    float t;
    t = pg_get_time();
    t = t / g->day_length;
    t = t - (int)t;
    return t;
}

float get_daylight(void) {
    float timer = time_of_day();
    if (timer < 0.5) {
        float t = (timer - 0.25) * 100;
        return 1 / (1 + powf(2, -t));
    }
    else {
        float t = (timer - 0.85) * 100;
        return 1 - 1 / (1 + powf(2, -t));
    }
}

float get_scale_factor(void) {
    return 1.0;
}

void get_sight_vector(float rx, float ry, float *vx, float *vy, float *vz) {
    float m = cosf(ry);
    *vx = cosf(rx - RADIANS(90)) * m;
    *vy = sinf(ry);
    *vz = sinf(rx - RADIANS(90)) * m;
}

void get_motion_vector(int flying, float sz, float sx, float rx, float ry,
    float *vx, float *vy, float *vz) {
    *vx = 0; *vy = 0; *vz = 0;
    if (!sz && !sx) {
        return;
    }
    if (flying) {
        float strafe = atan2f(sz, sx);
        float m = cosf(ry);
        float y = sinf(ry);
        if (sx) {
            if (!sz) {
                y = 0;
            }
            m = 1;
        }
        if (sz > 0) {
            y = -y;
        }
        *vx = cosf(rx + strafe) * m;
        *vy = y;
        *vz = sinf(rx + strafe) * m;
    } else {
        *vx = sx * cosf(rx) - sz * sinf(rx);
        *vz = sx * sinf(rx) + sz * cosf(rx);
    }
}

GLuint gen_crosshair_buffer(void) {
    int x = g->width / 2;
    int y = g->height / 2;
    int p = 10 * g->scale;
    float data[] = {
        x, y - p, x, y + p,
        x - p, y, x + p, y
    };
    return gen_buffer(sizeof(data), data);
}

GLuint gen_wireframe_buffer(float x, float y, float z, float n, float height) {
    float data[72];
    make_cube_wireframe(data, x, y, z, n, height);
    return gen_buffer(sizeof(data), data);
}

GLuint gen_sky_buffer(void) {
    float data[3072];
    GLint buffer;
    make_sphere(data, 1, 2);
    if (config->use_hfloat) {
        hfloat hdata[3072];
        for (size_t i=0; i<3072; i++) {
            hdata[i] = float_to_hfloat(data + i);
        }
        buffer = gen_buffer(sizeof(hdata), hdata);
    } else {
        buffer = gen_buffer(sizeof(data), data);
    }
    return buffer;
}

GLuint gen_cube_buffer(float x, float y, float z, float n, int w) {
    GLfloat *data = malloc_faces(10, 6, sizeof(GLfloat));
    float ao[6][4] = {0};
    float light[6][4] = {
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5},
        {0.5, 0.5, 0.5, 0.5}
    };
    GLint buffer;
    make_cube(data, ao, light, 1, 1, 1, 1, 1, 1, x, y, z, n, w);
    if (config->use_hfloat) {
        hfloat *hdata = malloc_faces(10, 6, sizeof(hfloat));
        for (size_t i=0; i<(6 * 10 * 6); i++) {
            hdata[i] = float_to_hfloat(data + i);
        }
        buffer = gen_faces(10, 6, hdata, sizeof(hfloat));
        free(data);
    } else {
        buffer = gen_faces(10, 6, data, sizeof(GLfloat));
    }
    return buffer;
}

GLuint gen_plant_buffer(float x, float y, float z, float n, int w) {
    GLfloat *data = malloc_faces(10, 4, sizeof(GLfloat));
    float ao = 0;
    float light = 1;
    GLint buffer;
    make_plant(data, ao, light, x, y, z, n, w, 45);
    if (config->use_hfloat) {
        hfloat *hdata = malloc_faces(10, 4, sizeof(hfloat));
        for (size_t i=0; i<(6 * 10 * 4); i++) {
            hdata[i] = float_to_hfloat(data + i);
        }
        buffer = gen_faces(10, 4, hdata, sizeof(hfloat));
        free(data);
    } else {
        buffer = gen_faces(10, 4, data, sizeof(GLfloat));
    }
    return buffer;
}

GLuint gen_player_buffer(float x, float y, float z, float rx, float ry, int p) {
    GLfloat *data = malloc_faces(10, 6, sizeof(GLfloat));
    make_player(data, x, y, z, rx, ry, p);
    return gen_faces(10, 6, data, sizeof(GLfloat));
}

GLuint gen_text_buffer(float x, float y, float n, char *text) {
    int length = strlen(text);
    GLfloat *data = malloc_faces(4, length, sizeof(GLfloat));
    for (int i = 0; i < length; i++) {
        make_character(data + i * 24, x, y, n / 2, n, text[i]);
        x += n;
    }
    return gen_faces(4, length, data, sizeof(GLfloat));
}

GLuint gen_mouse_cursor_buffer(float x, float y, int p) {
    GLfloat *data = malloc_faces(4, 1, sizeof(GLfloat));
    make_mouse_cursor(data, x, y, p);
    return gen_faces(4, 1, data, sizeof(GLfloat));
}

void draw_triangles_3d_ao(Attrib *attrib, GLuint buffer, int count,
                          size_t type_size, int gl_type) {
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glEnableVertexAttribArray(attrib->position);
    glEnableVertexAttribArray(attrib->normal);
    glEnableVertexAttribArray(attrib->uv);
    glVertexAttribPointer(attrib->position, 3, gl_type, GL_FALSE,
        type_size * 10, 0);
    glVertexAttribPointer(attrib->normal, 3, gl_type, GL_FALSE,
        type_size * 10, (GLvoid *)(type_size * 3));
    glVertexAttribPointer(attrib->uv, 4, gl_type, GL_FALSE,
        type_size * 10, (GLvoid *)(type_size * 6));
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDisableVertexAttribArray(attrib->position);
    glDisableVertexAttribArray(attrib->normal);
    glDisableVertexAttribArray(attrib->uv);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void draw_triangles_3d_text(Attrib *attrib, GLuint buffer, int count) {
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glEnableVertexAttribArray(attrib->position);
    glEnableVertexAttribArray(attrib->uv);
    glEnableVertexAttribArray(attrib->color);
    glVertexAttribPointer(attrib->position, 3, GL_FLOAT, GL_FALSE,
        sizeof(GLfloat) * 9, 0);
    glVertexAttribPointer(attrib->uv, 2, GL_FLOAT, GL_FALSE,
        sizeof(GLfloat) * 9, (GLvoid *)(sizeof(GLfloat) * 3));
    glVertexAttribPointer(attrib->color, 4, GL_FLOAT, GL_FALSE,
        sizeof(GLfloat) * 9, (GLvoid *)(sizeof(GLfloat) * 5));
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDisableVertexAttribArray(attrib->position);
    glDisableVertexAttribArray(attrib->uv);
    glDisableVertexAttribArray(attrib->color);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void draw_triangles_3d_sky(Attrib *attrib, GLuint buffer, int count) {
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glEnableVertexAttribArray(attrib->position);
    glEnableVertexAttribArray(attrib->uv);
    glVertexAttribPointer(attrib->position, 3, g->gl_float_type, GL_FALSE,
        g->float_size * 8, 0);
    glVertexAttribPointer(attrib->uv, 2, g->gl_float_type, GL_FALSE,
        g->float_size * 8, (GLvoid *)(g->float_size * 6));
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDisableVertexAttribArray(attrib->position);
    glDisableVertexAttribArray(attrib->uv);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void draw_triangles_2d(Attrib *attrib, GLuint buffer, int count) {
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glEnableVertexAttribArray(attrib->position);
    glEnableVertexAttribArray(attrib->uv);
    glVertexAttribPointer(attrib->position, 2, GL_FLOAT, GL_FALSE,
        sizeof(GLfloat) * 4, 0);
    glVertexAttribPointer(attrib->uv, 2, GL_FLOAT, GL_FALSE,
        sizeof(GLfloat) * 4, (GLvoid *)(sizeof(GLfloat) * 2));
    glDrawArrays(GL_TRIANGLES, 0, count);
    glDisableVertexAttribArray(attrib->position);
    glDisableVertexAttribArray(attrib->uv);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void draw_lines(Attrib *attrib, GLuint buffer, int components, int count) {
    glBindBuffer(GL_ARRAY_BUFFER, buffer);
    glEnableVertexAttribArray(attrib->position);
    glVertexAttribPointer(
        attrib->position, components, GL_FLOAT, GL_FALSE, 0, 0);
    glDrawArrays(GL_LINES, 0, count);
    glDisableVertexAttribArray(attrib->position);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void draw_chunk(Attrib *attrib, Chunk *chunk) {
    draw_triangles_3d_ao(attrib, chunk->buffer, chunk->faces * 6,
                         g->float_size, g->gl_float_type);
}

void draw_item(Attrib *attrib, GLuint buffer, int count, size_t type_size,
               int gl_type) {
    draw_triangles_3d_ao(attrib, buffer, count, type_size, gl_type);
}

void draw_text(Attrib *attrib, GLuint buffer, int length) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    draw_triangles_2d(attrib, buffer, length * 6);
    glDisable(GL_BLEND);
}

void draw_signs(Attrib *attrib, Chunk *chunk) {
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-1.0, -2.0);
    draw_triangles_3d_text(attrib, chunk->sign_buffer, chunk->sign_faces * 6);
    glDisable(GL_POLYGON_OFFSET_FILL);
}

void draw_sign(Attrib *attrib, GLuint buffer, int length) {
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(-8, -1024);
    draw_triangles_3d_text(attrib, buffer, length * 6);
    glDisable(GL_POLYGON_OFFSET_FILL);
}

void draw_cube(Attrib *attrib, GLuint buffer, size_t type_size, int gl_type) {
    draw_item(attrib, buffer, 36, type_size, gl_type);
}

void draw_plant(Attrib *attrib, GLuint buffer) {
    draw_item(attrib, buffer, 24, g->float_size, g->gl_float_type);
}

void draw_player(Attrib *attrib, Player *player) {
    draw_cube(attrib, player->buffer, sizeof(GLfloat), GL_FLOAT);
}

void draw_mouse(Attrib *attrib, GLuint buffer) {
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    draw_triangles_2d(attrib, buffer, 6);
    glDisable(GL_BLEND);
}

Client *find_client(int id) {
    for (int i = 0; i < g->client_count; i++) {
        Client *client = g->clients + i;
        if (client->id == id) {
            return client;
        }
    }
    return 0;
}

void update_player(Player *player,
    float x, float y, float z, float rx, float ry, int interpolate)
{
    if (interpolate) {
        State *s1 = &player->state1;
        State *s2 = &player->state2;
        memcpy(s1, s2, sizeof(State));
        s2->x = x; s2->y = y; s2->z = z; s2->rx = rx; s2->ry = ry;
        s2->t = pg_get_time();
        if (s2->rx - s1->rx > PI) {
            s1->rx += 2 * PI;
        }
        if (s1->rx - s2->rx > PI) {
            s1->rx -= 2 * PI;
        }
    }
    else {
        State *s = &player->state;
        s->x = x; s->y = y; s->z = z; s->rx = rx; s->ry = ry;
        del_buffer(player->buffer);
        player->buffer = gen_player_buffer(s->x, s->y, s->z, s->rx, s->ry,
                                           player->texture_index);
    }
}

void interpolate_player(Player *player) {
    State *s1 = &player->state1;
    State *s2 = &player->state2;
    float t1 = s2->t - s1->t;
    float t2 = pg_get_time() - s2->t;
    t1 = MIN(t1, 1);
    t1 = MAX(t1, 0.1);
    float p = MIN(t2 / t1, 1);
    update_player(
        player,
        s1->x + (s2->x - s1->x) * p,
        s1->y + (s2->y - s1->y) * p,
        s1->z + (s2->z - s1->z) * p,
        s1->rx + (s2->rx - s1->rx) * p,
        s1->ry + (s2->ry - s1->ry) * p,
        0);
}

void delete_client(int id) {
    Client *client = find_client(id);
    if (!client) {
        return;
    }
    int count = g->client_count;
    for (int i = 0; i<MAX_LOCAL_PLAYERS; i++) {
        Player *player = client->players + i;
        if (player->is_active) {
            del_buffer(player->buffer);
        }
    }
    Client *other = g->clients + (--count);
    memcpy(client, other, sizeof(Client));
    g->client_count = count;
}

void delete_all_players(void) {
    for (int i = 0; i < g->client_count; i++) {
        Client *client = g->clients + i;
        for (int j = 0; j < MAX_LOCAL_PLAYERS; j++) {
            Player *player = client->players + j;
            if (player->is_active) {
                del_buffer(player->buffer);
            }
        }
    }
    g->client_count = 0;
}

float player_player_distance(Player *p1, Player *p2) {
    State *s1 = &p1->state;
    State *s2 = &p2->state;
    float x = s2->x - s1->x;
    float y = s2->y - s1->y;
    float z = s2->z - s1->z;
    return sqrtf(x * x + y * y + z * z);
}

float player_crosshair_distance(Player *p1, Player *p2) {
    State *s1 = &p1->state;
    State *s2 = &p2->state;
    float d = player_player_distance(p1, p2);
    float vx, vy, vz;
    get_sight_vector(s1->rx, s1->ry, &vx, &vy, &vz);
    vx *= d; vy *= d; vz *= d;
    float px, py, pz;
    px = s1->x + vx; py = s1->y + vy; pz = s1->z + vz;
    float x = s2->x - px;
    float y = s2->y - py;
    float z = s2->z - pz;
    return sqrtf(x * x + y * y + z * z);
}

Player *player_crosshair(Player *player) {
    Player *result = 0;
    float threshold = RADIANS(5);
    float best = 0;
    for (int i = 0; i < g->client_count; i++) {
        Client *client = g->clients + i;
        for (int j = 0; j < MAX_LOCAL_PLAYERS; j++) {
            Player *other = client->players + j;
            if (other == player || !other->is_active) {
                continue;
            }
            float p = player_crosshair_distance(player, other);
            float d = player_player_distance(player, other);
            if (d < 96 && p / d < threshold) {
                if (best == 0 || d < best) {
                    best = d;
                    result = other;
                }
            }
        }
    }
    return result;
}

Chunk *find_chunk(int p, int q) {
    for (int i = 0; i < g->chunk_count; i++) {
        Chunk *chunk = g->chunks + i;
        if (chunk->p == p && chunk->q == q) {
            return chunk;
        }
    }
    return 0;
}

int chunk_distance(Chunk *chunk, int p, int q) {
    int dp = ABS(chunk->p - p);
    int dq = ABS(chunk->q - q);
    return MAX(dp, dq);
}

int chunk_visible(float planes[6][4], int p, int q, int miny, int maxy) {
    int x = p * CHUNK_SIZE - 1;
    int z = q * CHUNK_SIZE - 1;
    int d = CHUNK_SIZE + 1;
    float points[8][3] = {
        {x + 0, miny, z + 0},
        {x + d, miny, z + 0},
        {x + 0, miny, z + d},
        {x + d, miny, z + d},
        {x + 0, maxy, z + 0},
        {x + d, maxy, z + 0},
        {x + 0, maxy, z + d},
        {x + d, maxy, z + d}
    };
    int n = g->ortho ? 4 : 6;
    for (int i = 0; i < n; i++) {
        int in = 0;
        int out = 0;
        for (int j = 0; j < 8; j++) {
            float d =
                planes[i][0] * points[j][0] +
                planes[i][1] * points[j][1] +
                planes[i][2] * points[j][2] +
                planes[i][3];
            if (d < 0) {
                out++;
            }
            else {
                in++;
            }
            if (in && out) {
                break;
            }
        }
        if (in == 0) {
            return 0;
        }
    }
    return 1;
}

int highest_block(float x, float z) {
    int result = -1;
    int nx = roundf(x);
    int nz = roundf(z);
    int p = chunked(x);
    int q = chunked(z);
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        Map *map = &chunk->map;
        MAP_FOR_EACH(map, ex, ey, ez, ew) {
            if (is_obstacle(ew, 0, 0) && ex == nx && ez == nz) {
                result = MAX(result, ey);
            }
        } END_MAP_FOR_EACH;
    }
    return result;
}

int _hit_test(
    Map *map, float max_distance, int previous,
    float x, float y, float z,
    float vx, float vy, float vz,
    int *hx, int *hy, int *hz)
{
    int m = 32;
    int px = 0;
    int py = 0;
    int pz = 0;
    for (int i = 0; i < max_distance * m; i++) {
        int nx = roundf(x);
        int ny = roundf(y);
        int nz = roundf(z);
        if (nx != px || ny != py || nz != pz) {
            int hw = map_get(map, nx, ny, nz);
            if (hw > 0) {
                if (previous) {
                    *hx = px; *hy = py; *hz = pz;
                }
                else {
                    *hx = nx; *hy = ny; *hz = nz;
                }
                return hw;
            }
            px = nx; py = ny; pz = nz;
        }
        x += vx / m; y += vy / m; z += vz / m;
    }
    return 0;
}

int hit_test(
    int previous, float x, float y, float z, float rx, float ry,
    int *bx, int *by, int *bz)
{
    int result = 0;
    float best = 0;
    int p = chunked(x);
    int q = chunked(z);
    float vx, vy, vz;
    get_sight_vector(rx, ry, &vx, &vy, &vz);
    for (int i = 0; i < g->chunk_count; i++) {
        Chunk *chunk = g->chunks + i;
        if (chunk_distance(chunk, p, q) > 1) {
            continue;
        }
        int hx, hy, hz;
        int hw = _hit_test(&chunk->map, 8, previous,
            x, y, z, vx, vy, vz, &hx, &hy, &hz);
        if (hw > 0) {
            float d = sqrtf(
                powf(hx - x, 2) + powf(hy - y, 2) + powf(hz - z, 2));
            if (best == 0 || d < best) {
                best = d;
                *bx = hx; *by = hy; *bz = hz;
                result = hw;
            }
        }
    }
    return result;
}

int hit_test_face(Player *player, int *x, int *y, int *z, int *face) {
    State *s = &player->state;
    int w = hit_test(0, s->x, s->y, s->z, s->rx, s->ry, x, y, z);
    if (is_obstacle(w, 0, 0)) {
        int hx, hy, hz;
        hit_test(1, s->x, s->y, s->z, s->rx, s->ry, &hx, &hy, &hz);
        int dx = hx - *x;
        int dy = hy - *y;
        int dz = hz - *z;
        if (dx == -1 && dy == 0 && dz == 0) {
            *face = 0; return 1;
        }
        if (dx == 1 && dy == 0 && dz == 0) {
            *face = 1; return 1;
        }
        if (dx == 0 && dy == 0 && dz == -1) {
            *face = 2; return 1;
        }
        if (dx == 0 && dy == 0 && dz == 1) {
            *face = 3; return 1;
        }
        if (dx == 0 && dy == 1 && dz == 0) {
            int degrees = roundf(DEGREES(atan2f(s->x - hx, s->z - hz)));
            if (degrees < 0) {
                degrees += 360;
            }
            int top = ((degrees + 45) / 90) % 4;
            *face = 4 + top; return 1;
        }
    }
    return 0;
}

int collide(int height, float *x, float *y, float *z, float *ydiff) {
    #define AUTO_JUMP_LIMIT 0.5
    int result = 0;
    int p = chunked(*x);
    int q = chunked(*z);
    Chunk *chunk = find_chunk(p, q);
    if (!chunk) {
        return result;
    }
    Map *map = &chunk->map;
    Map *shape_map = &chunk->shape;
    Map *extra_map = &chunk->extra;
    int nx = roundf(*x);
    int ny = roundf(*y);
    int nz = roundf(*z);
    float px = *x - nx;
    float py = *y - ny;
    float pz = *z - nz;
    float pad = 0.25;
    uint8_t coll_ok = 1;
    uint8_t need_jump = 0;
    for (int dy = 0; dy < height; dy++) {
        if (px < -pad && is_obstacle(map_get(map, nx - 1, ny - dy, nz),
                                     map_get(shape_map, nx - 1, ny - dy, nz),
                                     map_get(extra_map, nx - 1, ny - dy, nz))) {
            *x = nx - pad;
            if (dy == 0) {
                coll_ok = 0;
            } else if (coll_ok &&
                       item_height(map_get(shape_map, nx - 1, ny - dy, nz)) <= AUTO_JUMP_LIMIT) {
                need_jump = 1;
            }
        }
        if (px > pad && is_obstacle(map_get(map, nx + 1, ny - dy, nz),
                                    map_get(shape_map, nx + 1, ny - dy, nz),
                                    map_get(extra_map, nx + 1, ny - dy, nz))) {
            *x = nx + pad;
            if (dy == 0) {
                coll_ok = 0;
            } else if (coll_ok &&
                       item_height(map_get(shape_map, nx + 1, ny - dy, nz)) <= AUTO_JUMP_LIMIT) {
                need_jump = 1;
            }
        }
        if (py < -pad && is_obstacle(map_get(map, nx, ny - dy - 1, nz),
                                     map_get(shape_map, nx, ny - dy - 1, nz),
                                     map_get(extra_map, nx, ny - dy - 1, nz))) {
            *y = ny - pad;
            result = 1;
        }
        if (py > pad && is_obstacle(map_get(map, nx, ny - dy + 1, nz),
                                    map_get(shape_map, nx, ny - dy + 1, nz),
                                    map_get(extra_map, nx, ny - dy + 1, nz))) {
            // reached when player jumps and hits their head on block above
            *y = ny + pad;
            result = 1;
        }
        if (pz < -pad && is_obstacle(map_get(map, nx, ny - dy, nz - 1),
                                     map_get(shape_map, nx, ny - dy, nz - 1),
                                     map_get(extra_map, nx, ny - dy, nz - 1))) {
            *z = nz - pad;
            if (dy == 0) {
                coll_ok = 0;
            } else if (coll_ok &&
                       item_height(map_get(shape_map, nx, ny - dy, nz - 1)) <= AUTO_JUMP_LIMIT) {
                need_jump = 1;
            }
        }
        if (pz > pad && is_obstacle(map_get(map, nx, ny - dy, nz + 1),
                                    map_get(shape_map, nx, ny - dy, nz + 1),
                                    map_get(extra_map, nx, ny - dy, nz + 1))) {
            *z = nz + pad;
            if (dy == 0) {
                coll_ok = 0;
            } else if (coll_ok &&
                       item_height(map_get(shape_map, nx, ny - dy, nz + 1)) <= AUTO_JUMP_LIMIT) {
                need_jump = 1;
            }
        }

        // check the 4 diagonally neighboring blocks for obstacle as well
        if (px < -pad && pz > pad &&
            is_obstacle(map_get(map, nx - 1, ny - dy, nz + 1),
                        map_get(shape_map, nx - 1, ny - dy, nz + 1),
                        map_get(extra_map, nx - 1, ny - dy, nz + 1))) {
            if(ABS(px) < ABS(pz)) {
                *x = nx - pad;
            } else {
                *z = nz + pad;
            }
        }
        if (px > pad && pz > pad &&
            is_obstacle(map_get(map, nx + 1, ny - dy, nz + 1),
                        map_get(shape_map, nx + 1, ny - dy, nz + 1),
                        map_get(extra_map, nx + 1, ny - dy, nz + 1))) {
            if(ABS(px) < ABS(pz)) {
                *x = nx + pad;
            } else {
                *z = nz + pad;
            }
        }
        if (px < -pad && pz < -pad &&
            is_obstacle(map_get(map, nx - 1, ny - dy, nz - 1),
                        map_get(shape_map, nx - 1, ny - dy, nz - 1),
                        map_get(extra_map, nx - 1, ny - dy, nz - 1))) {
            if(ABS(px) < ABS(pz)) {
                *x = nx - pad;
            } else {
                *z = nz - pad;
            }
        }
        if (px > pad && pz < -pad &&
            is_obstacle(map_get(map, nx + 1, ny - dy, nz - 1),
                        map_get(shape_map, nx + 1, ny - dy, nz - 1),
                        map_get(extra_map, nx + 1, ny - dy, nz - 1))) {
            if(ABS(px) < ABS(pz)) {
                *x = nx + pad;
            } else {
                *z = nz - pad;
            }
        }
    }
    /* If there's no block at head height, a block at foot height, and
       standing on firm ground, then autojump */
    if (need_jump && result == 1) {
        *ydiff = 8;
        result = 0;
    }
    return result;
}

int player_intersects_block(
    int height,
    float x, float y, float z,
    int hx, int hy, int hz)
{
    int nx = roundf(x);
    int ny = roundf(y);
    int nz = roundf(z);
    for (int i = 0; i < height; i++) {
        if (nx == hx && ny - i == hy && nz == hz) {
            return 1;
        }
    }
    return 0;
}

int _gen_sign_buffer(
    GLfloat *data, float x, float y, float z, int face, const char *text)
{
    static const int glyph_dx[8] = {0, 0, -1, 1, 1, 0, -1, 0};
    static const int glyph_dz[8] = {1, -1, 0, 0, 0, -1, 0, 1};
    static const int line_dx[8] = {0, 0, 0, 0, 0, 1, 0, -1};
    static const int line_dy[8] = {-1, -1, -1, -1, 0, 0, 0, 0};
    static const int line_dz[8] = {0, 0, 0, 0, 1, 0, -1, 0};
    if (face < 0 || face >= 8) {
        return 0;
    }

    // Align sign to the item shape
    int shape = get_shape(x, y, z);
    float y_face_height = item_height(shape);
    float face_height_offset = 0;
    if (face >= 0 && face <= 3) { // side faces
        face_height_offset = 0.5 - (y_face_height / 2);
    } else if (face >= 4 && face <= 7) { // top faces
        face_height_offset = 1 - y_face_height;
    }

    float font_scaling = 1.0;
    float r = 0.0, g = 0.0, b = 0.0;
    if (strlen(text) > 2 && text[0] == '\\' &&
        (isdigit(text[1]) || text[1] == '.')) {
        font_scaling = atof(&text[1]);
    }
    int count = 0;
    float max_width = 64 / font_scaling;
    float line_height = 1.25;
    char lines[1024];
    int rows = wrap(text, max_width, lines, 1024);
    rows = MIN(rows, 5);
    int dx = glyph_dx[face];
    int dz = glyph_dz[face];
    int ldx = line_dx[face];
    int ldy = line_dy[face];
    int ldz = line_dz[face];
    float n = 1.0 / (max_width / 10);
    float sx = x - n * (rows - 1) * (line_height / 2) * ldx;
    float sy = y - n * (rows - 1) * (line_height / 2) * ldy -
               face_height_offset;
    float sz = z - n * (rows - 1) * (line_height / 2) * ldz;
    char *key;
    char *line = tokenize(lines, "\n", &key);
    while (line) {
        int length = strlen(line);
        int line_start = 0;
        int line_width = string_width(line + line_start);
        line_width = MIN(line_width, max_width);
        float rx = sx - dx * line_width / max_width / 2;
        float ry = sy;
        float rz = sz - dz * line_width / max_width / 2;
        for (int i = line_start; i < length; i++) {
            if (line[i] == '\\' && i+1 < length) {
                // process markup
                char color_text[MAX_COLOR_STRING_LENGTH];
                if (i+7 < length && line[i+1] == '#' && isxdigit(line[i+2]) &&
                    isxdigit(line[i+3]) && isxdigit(line[i+4]) &&
                    isxdigit(line[i+5]) && isxdigit(line[i+6]) &&
                    isxdigit(line[i+7])) {
                    strncpy(color_text, line + i + 1, 7);
                    color_text[MAX_COLOR_STRING_LENGTH - 1] = '\0';
                    color_text[7] = '\0';
                    color_from_text(color_text, &r, &g, &b);
                } else if (i+4 < length && line[i+1] == '#' &&
                           isxdigit(line[i+2]) && isxdigit(line[i+3]) &&
                           isxdigit(line[i+4])) {
                    strncpy(color_text, line + i + 1, 4);
                    color_text[MAX_COLOR_STRING_LENGTH - 1] = '\0';
                    color_text[4] = '\0';
                    color_from_text(color_text, &r, &g, &b);
                } else if (isalpha(line[i+1])) {
                    strncpy(color_text, line + i + 1, 1);
                    color_text[MAX_COLOR_STRING_LENGTH - 1] = '\0';
                    color_text[1] = '\0';
                    color_from_text(color_text, &r, &g, &b);
                }
                // eat all remaining markup text
                while (line[i] != ' ' && i < length) {
                    i++;
                }
                continue;  // do not process markup as displayable text
            }
            int width = char_width(line[i]);
            line_width -= width;
            if (line_width < 0) {
                break;
            }
            rx += dx * width / max_width / 2;
            rz += dz * width / max_width / 2;
            if (line[i] != ' ') {
                make_character_3d(
                    data + count * 54, rx, ry, rz, n / 2, face, line[i],
                    r, g, b);
                count++;
            }
            rx += dx * width / max_width / 2;
            rz += dz * width / max_width / 2;
        }
        sx += n * line_height * ldx;
        sy += n * line_height * ldy;
        sz += n * line_height * ldz;
        line = tokenize(NULL, "\n", &key);
        rows--;
        if (rows <= 0) {
            break;
        }
    }
    return count;
}

void gen_sign_buffer(Chunk *chunk) {
    SignList *signs = &chunk->signs;

    // first pass - count characters
    int max_faces = 0;
    for (size_t i = 0; i < signs->size; i++) {
        Sign *e = signs->data + i;
        max_faces += strlen(e->text);
    }

    // second pass - generate geometry
    GLfloat *data = malloc_faces_with_rgba(5, max_faces);
    int faces = 0;
    for (size_t i = 0; i < signs->size; i++) {
        Sign *e = signs->data + i;
        faces += _gen_sign_buffer(
            data + faces * 54, e->x, e->y, e->z, e->face, e->text);
    }

    del_buffer(chunk->sign_buffer);
    chunk->sign_buffer = gen_faces_with_rgba(5, faces, data);
    chunk->sign_faces = faces;
    chunk->dirty_signs = 0;
}

int has_lights(Chunk *chunk) {
    if (!config->show_lights) {
        return 0;
    }
    for (int dp = -1; dp <= 1; dp++) {
        for (int dq = -1; dq <= 1; dq++) {
            Chunk *other = chunk;
            if (dp || dq) {
                other = find_chunk(chunk->p + dp, chunk->q + dq);
            }
            if (!other) {
                continue;
            }
            Map *map = &other->lights;
            if (map->size) {
                return 1;
            }
        }
    }
    return 0;
}

void dirty_chunk(Chunk *chunk) {
    chunk->dirty = 1;
    chunk->dirty_signs = 1;
    if (has_lights(chunk)) {
        for (int dp = -1; dp <= 1; dp++) {
            for (int dq = -1; dq <= 1; dq++) {
                Chunk *other = find_chunk(chunk->p + dp, chunk->q + dq);
                if (other) {
                    other->dirty = 1;
                }
            }
        }
    }
}

void occlusion(
    char neighbors[27], char lights[27], float shades[27],
    float ao[6][4], float light[6][4])
{
    static const int lookup3[6][4][3] = {
        {{0, 1, 3}, {2, 1, 5}, {6, 3, 7}, {8, 5, 7}},
        {{18, 19, 21}, {20, 19, 23}, {24, 21, 25}, {26, 23, 25}},
        {{6, 7, 15}, {8, 7, 17}, {24, 15, 25}, {26, 17, 25}},
        {{0, 1, 9}, {2, 1, 11}, {18, 9, 19}, {20, 11, 19}},
        {{0, 3, 9}, {6, 3, 15}, {18, 9, 21}, {24, 15, 21}},
        {{2, 5, 11}, {8, 5, 17}, {20, 11, 23}, {26, 17, 23}}
    };
   static const int lookup4[6][4][4] = {
        {{0, 1, 3, 4}, {1, 2, 4, 5}, {3, 4, 6, 7}, {4, 5, 7, 8}},
        {{18, 19, 21, 22}, {19, 20, 22, 23}, {21, 22, 24, 25}, {22, 23, 25, 26}},
        {{6, 7, 15, 16}, {7, 8, 16, 17}, {15, 16, 24, 25}, {16, 17, 25, 26}},
        {{0, 1, 9, 10}, {1, 2, 10, 11}, {9, 10, 18, 19}, {10, 11, 19, 20}},
        {{0, 3, 9, 12}, {3, 6, 12, 15}, {9, 12, 18, 21}, {12, 15, 21, 24}},
        {{2, 5, 11, 14}, {5, 8, 14, 17}, {11, 14, 20, 23}, {14, 17, 23, 26}}
    };
    static const float curve[4] = {0.0, 0.25, 0.5, 0.75};
    for (int i = 0; i < 6; i++) {
        for (int j = 0; j < 4; j++) {
            int corner = neighbors[lookup3[i][j][0]];
            int side1 = neighbors[lookup3[i][j][1]];
            int side2 = neighbors[lookup3[i][j][2]];
            int value = side1 && side2 ? 3 : corner + side1 + side2;
            float shade_sum = 0;
            float light_sum = 0;
            int is_light = lights[13] == 15;
            for (int k = 0; k < 4; k++) {
                shade_sum += shades[lookup4[i][j][k]];
                light_sum += lights[lookup4[i][j][k]];
            }
            if (is_light) {
                light_sum = 15 * 4 * 10;
            }
            float total = curve[value] + shade_sum / 4.0;
            ao[i][j] = MIN(total, 1.0);
            light[i][j] = light_sum / 15.0 / 4.0;
        }
    }
}

#define XZ_SIZE (CHUNK_SIZE * 3 + 2)
#define XZ_LO (CHUNK_SIZE)
#define XZ_HI (CHUNK_SIZE * 2 + 1)
#define Y_SIZE 258
#define XYZ(x, y, z) ((y) * XZ_SIZE * XZ_SIZE + (x) * XZ_SIZE + (z))
#define XZ(x, z) ((x) * XZ_SIZE + (z))

void light_fill(
    char *opaque, char *light,
    int x, int y, int z, int w, int force)
{
    if (x + w < XZ_LO || z + w < XZ_LO) {
        return;
    }
    if (x - w > XZ_HI || z - w > XZ_HI) {
        return;
    }
    if (y < 0 || y >= Y_SIZE) {
        return;
    }
    if (light[XYZ(x, y, z)] >= w) {
        return;
    }
    if (!force && opaque[XYZ(x, y, z)]) {
        return;
    }
    light[XYZ(x, y, z)] = w--;
    light_fill(opaque, light, x - 1, y, z, w, 0);
    light_fill(opaque, light, x + 1, y, z, w, 0);
    light_fill(opaque, light, x, y - 1, z, w, 0);
    light_fill(opaque, light, x, y + 1, z, w, 0);
    light_fill(opaque, light, x, y, z - 1, w, 0);
    light_fill(opaque, light, x, y, z + 1, w, 0);
}

void compute_chunk(WorkerItem *item) {
    char *opaque = (char *)calloc(XZ_SIZE * XZ_SIZE * Y_SIZE, sizeof(char));
    char *light = (char *)calloc(XZ_SIZE * XZ_SIZE * Y_SIZE, sizeof(char));
    char *highest = (char *)calloc(XZ_SIZE * XZ_SIZE, sizeof(char));

    int ox = item->p * CHUNK_SIZE - CHUNK_SIZE - 1;
    int oy = -1;
    int oz = item->q * CHUNK_SIZE - CHUNK_SIZE - 1;

    // check for shapes
    Map *shape_map = item->shape_maps[1][1];
    int has_shape = 0;
    for (int a = 0; a < 3; a++) {
        for (int b = 0; b < 3; b++) {
            Map *map = item->shape_maps[a][b];
            if (map && map->size) {
                has_shape = 1;
            }
        }
    }

    // check for extra
    Map *extra_map = item->extra_maps[1][1];
    int has_extra = 0;
    for (int a = 0; a < 3; a++) {
        for (int b = 0; b < 3; b++) {
            Map *map = item->extra_maps[a][b];
            if (map && map->size) {
                has_extra = 1;
            }
        }
    }

    // check for lights
    int has_light = 0;
    if (config->show_lights) {
        for (int a = 0; a < 3; a++) {
            for (int b = 0; b < 3; b++) {
                Map *map = item->light_maps[a][b];
                if (map && map->size) {
                    has_light = 1;
                }
            }
        }
    }

    // populate opaque array
    for (int a = 0; a < 3; a++) {
        for (int b = 0; b < 3; b++) {
            Map *map = item->block_maps[a][b];
            if (!map) {
                continue;
            }
            if (has_shape) {
                shape_map = item->shape_maps[a][b];
            }
            MAP_FOR_EACH(map, ex, ey, ez, ew) {
                int x = ex - ox;
                int y = ey - oy;
                int z = ez - oz;
                int w = ew;
                // TODO: this should be unnecessary
                if (x < 0 || y < 0 || z < 0) {
                    continue;
                }
                if (x >= XZ_SIZE || y >= Y_SIZE || z >= XZ_SIZE) {
                    continue;
                }
                // END TODO
                opaque[XYZ(x, y, z)] = !is_transparent(w);
                if (has_shape && shape_map && map_get(shape_map, ex, ey, ez)) {
                    opaque[XYZ(x, y, z)] = 0;
                }
                if (opaque[XYZ(x, y, z)]) {
                    highest[XZ(x, z)] = MAX(highest[XZ(x, z)], y);
                }
            } END_MAP_FOR_EACH;
        }
    }

    // flood fill light intensities
    if (has_light) {
        for (int a = 0; a < 3; a++) {
            for (int b = 0; b < 3; b++) {
                Map *map = item->light_maps[a][b];
                if (!map) {
                    continue;
                }
                MAP_FOR_EACH(map, ex, ey, ez, ew) {
                    int x = ex - ox;
                    int y = ey - oy;
                    int z = ez - oz;
                    light_fill(opaque, light, x, y, z, ew, 1);
                } END_MAP_FOR_EACH;
            }
        }
    }

    Map *map = item->block_maps[1][1];
    if (has_shape) {
        shape_map = item->shape_maps[1][1];
    }
    int has_transform = 0;
    Map *transform_map = item->transform_maps[1][1];
    if (transform_map && transform_map->size) {
        has_transform = 1;
    }
    DoorMap *door_map = item->door_maps[1][1];

    // count exposed faces
    int miny = 256;
    int maxy = 0;
    int faces = 0;
    MAP_FOR_EACH(map, ex, ey, ez, ew) {
        if (ew <= 0) {
            continue;
        }
        int x = ex - ox;
        int y = ey - oy;
        int z = ez - oz;
        int f1 = !opaque[XYZ(x - 1, y, z)];
        int f2 = !opaque[XYZ(x + 1, y, z)];
        int f3 = !opaque[XYZ(x, y + 1, z)];
        int f4 = !opaque[XYZ(x, y - 1, z)] && (ey > 0);
        int f5 = !opaque[XYZ(x, y, z - 1)];
        int f6 = !opaque[XYZ(x, y, z + 1)];
        int total = f1 + f2 + f3 + f4 + f5 + f6;
        if (total == 0) {
            continue;
        }
        if (is_plant(ew)) {
            total = 4;
        } else if (has_shape && shape_map) {
            int shape = map_get(shape_map, ex, ey, ez);
            if (shape) {
                if (shape >= SLAB1 && shape <= SLAB15) {
                    // Top face of slab is viewable when a block is above the slab.
                    f3 = 1;
                    total = f1 + f2 + f3 + f4 + f5 + f6;
                } else if (shape == UPPER_DOOR || shape == LOWER_DOOR) {
                    // Different side faces of a door may be visible when open.
                    f1 = 1;
                    f2 = 1;
                    f5 = 1;
                    f6 = 1;
                    total = f1 + f2 + f3 + f4 + f5 + f6;
                } else if (shape >= FENCE && shape <= GATE) {
                    // Hidden face removal not yet enabled for fence shapes.
                    total = fence_face_count(shape);
                }
            }
        }
        miny = MIN(miny, ey);
        maxy = MAX(maxy, ey);
        faces += total;
    } END_MAP_FOR_EACH;

    // generate geometry
    GLfloat *data = malloc_faces(10, faces, sizeof(GLfloat));
    int offset = 0;
    MAP_FOR_EACH(map, ex, ey, ez, ew) {
        if (ew <= 0) {
            continue;
        }
        int x = ex - ox;
        int y = ey - oy;
        int z = ez - oz;
        int f1 = !opaque[XYZ(x - 1, y, z)];
        int f2 = !opaque[XYZ(x + 1, y, z)];
        int f3 = !opaque[XYZ(x, y + 1, z)];
        int f4 = !opaque[XYZ(x, y - 1, z)] && (ey > 0);
        int f5 = !opaque[XYZ(x, y, z - 1)];
        int f6 = !opaque[XYZ(x, y, z + 1)];
        int total = f1 + f2 + f3 + f4 + f5 + f6;
        if (total == 0) {
            continue;
        }
        if (has_shape && shape_map) {
            int shape = map_get(shape_map, ex, ey, ez);
            if (shape >= SLAB1 && shape <= SLAB15) {
                // Top face of slab is viewable when a block is above the slab.
                f3 = 1;
                total = f1 + f2 + f3 + f4 + f5 + f6;
            } else if (shape == UPPER_DOOR || shape == LOWER_DOOR) {
                // Different side faces of a door may be visible when open.
                f1 = 1;
                f2 = 1;
                f5 = 1;
                f6 = 1;
                total = f1 + f2 + f3 + f4 + f5 + f6;
            } else if (shape >= FENCE && shape <= GATE) {
                f1 = 1; f2 = 1; f3 = 1; f4 = 1; f5 = 1; f6 = 1;
                total = fence_face_count(shape);
            }
        }
        char neighbors[27] = {0};
        char lights[27] = {0};
        float shades[27] = {0};
        int index = 0;
        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                for (int dz = -1; dz <= 1; dz++) {
                    neighbors[index] = opaque[XYZ(x + dx, y + dy, z + dz)];
                    lights[index] = light[XYZ(x + dx, y + dy, z + dz)];
                    shades[index] = 0;
                    if (y + dy <= highest[XZ(x + dx, z + dz)]) {
                        for (int oy = 0; oy < 8; oy++) {
                            if (opaque[XYZ(x + dx, y + dy + oy, z + dz)]) {
                                shades[index] = 1.0 - oy * 0.125;
                                break;
                            }
                        }
                    }
                    index++;
                }
            }
        }
        float ao[6][4];
        float light[6][4];
        occlusion(neighbors, lights, shades, ao, light);
        if (is_plant(ew)) {
            total = 4;
            float min_ao = 1;
            float max_light = 0;
            for (int a = 0; a < 6; a++) {
                for (int b = 0; b < 4; b++) {
                    min_ao = MIN(min_ao, ao[a][b]);
                    max_light = MAX(max_light, light[a][b]);
                }
            }
            float rotation = simplex2(ex, ez, 4, 0.5, 2) * 360;
            make_plant(
                data + offset, min_ao, max_light,
                entry->e.x, entry->e.y, entry->e.z, 0.5, ew, rotation);
        }
        else if (has_shape && shape_map) {
            int shape = map_get(shape_map, ex, ey, ez);
            if (shape) {
                int transform = 0;
                if (has_transform) {
                    transform = map_get(transform_map, ex, ey, ez);
                }
                if (shape >= SLAB1 && shape <= SLAB15) {
                    make_slab(
                        data + offset, ao, light,
                        f1, f2, f3, f4, f5, f6,
                        entry->e.x, entry->e.y, entry->e.z, 0.5, ew, shape);
                } else if (shape == LOWER_DOOR || shape == UPPER_DOOR) {
                    int extra = 0;
                    if (has_extra && extra_map)  {
                        extra = map_get(extra_map, ex, ey, ez);
                    }
                    door_map_set(door_map, ex, ey, ez, ew, offset, total, ao,
                        light, f1, f2, f3, f4, f5, f6, 0.5, shape, extra,
                        transform);
                    make_door(
                        data + offset, ao, light,
                        f1, f2, f3, f4, f5, f6,
                        entry->e.x, entry->e.y, entry->e.z, 0.5, ew, shape,
                        extra, transform);
                } else if (shape >= FENCE && shape <= GATE) {
                    int extra = 0;
                    if (has_extra && extra_map)  {
                        extra = map_get(extra_map, ex, ey, ez);
                    }
                    if (shape == GATE) {
                        door_map_set(door_map, ex, ey, ez, ew, offset, total, ao,
                            light, f1, f2, f3, f4, f5, f6, 0.5, shape, extra,
                            transform);
                    }
                    make_fence(data + offset, ao, light,
                        f1, f2, f3, f4, f5, f6,
                        entry->e.x, entry->e.y, entry->e.z, 0.5, ew, shape,
                        extra, transform);
                }

            } else {
                make_cube(
                    data + offset, ao, light,
                    f1, f2, f3, f4, f5, f6,
                    entry->e.x, entry->e.y, entry->e.z, 0.5, ew);
            }
        }
        else {
            make_cube(
                data + offset, ao, light,
                f1, f2, f3, f4, f5, f6,
                entry->e.x, entry->e.y, entry->e.z, 0.5, ew);
        }
        offset += total * 60;
    } END_MAP_FOR_EACH;

    free(opaque);
    free(light);
    free(highest);

    item->miny = miny;
    item->maxy = maxy;
    item->faces = faces;
    if (config->use_hfloat) {
        hfloat *hdata = malloc_faces(10, faces, sizeof(hfloat));
        for (int i=0; i < (6 * 10 * faces); i++) {
            hdata[i] = float_to_hfloat(data + i);
        }
        free(data);
        item->data = hdata;
    } else {
        item->data = data;
    }
}

void generate_chunk(Chunk *chunk, WorkerItem *item) {
    chunk->miny = item->miny;
    chunk->maxy = item->maxy;
    chunk->faces = item->faces;
    del_buffer(chunk->buffer);
    chunk->buffer = gen_faces(10, item->faces, item->data, g->float_size);
    gen_sign_buffer(chunk);
}

void gen_chunk_buffer(Chunk *chunk) {
    WorkerItem _item;
    WorkerItem *item = &_item;
    item->p = chunk->p;
    item->q = chunk->q;
    for (int dp = -1; dp <= 1; dp++) {
        for (int dq = -1; dq <= 1; dq++) {
            Chunk *other = chunk;
            if (dp || dq) {
                other = find_chunk(chunk->p + dp, chunk->q + dq);
            }
            if (other) {
                item->block_maps[dp + 1][dq + 1] = &other->map;
                item->extra_maps[dp + 1][dq + 1] = &other->extra;
                item->light_maps[dp + 1][dq + 1] = &other->lights;
                item->shape_maps[dp + 1][dq + 1] = &other->shape;
                item->transform_maps[dp + 1][dq + 1] = &other->transform;
                item->door_maps[dp + 1][dq + 1] = &other->doors;
            }
            else {
                item->block_maps[dp + 1][dq + 1] = 0;
                item->extra_maps[dp + 1][dq + 1] = 0;
                item->light_maps[dp + 1][dq + 1] = 0;
                item->shape_maps[dp + 1][dq + 1] = 0;
                item->transform_maps[dp + 1][dq + 1] = 0;
                item->door_maps[dp + 1][dq + 1] = 0;
            }
        }
    }
    compute_chunk(item);
    generate_chunk(chunk, item);
    chunk->dirty = 0;
}

void map_set_func(int x, int y, int z, int w, void *arg) {
    Map *map = (Map *)arg;
    map_set(map, x, y, z, w);
}

void load_chunk(WorkerItem *item, lua_State *L) {
    int p = item->p;
    int q = item->q;
    Map *block_map = item->block_maps[1][1];
    Map *extra_map = item->extra_maps[1][1];
    Map *light_map = item->light_maps[1][1];
    Map *shape_map = item->shape_maps[1][1];
    Map *transform_map = item->transform_maps[1][1];
    SignList *signs = &item->signs;
    sign_list_alloc(signs, 16);
    if (L != NULL) {
        pwlua_worldgen(L, p, q, block_map, extra_map, light_map, shape_map, signs, transform_map);
    } else {
        create_world(p, q, map_set_func, block_map);
    }
    db_load_blocks(block_map, p, q);
    db_load_extras(extra_map, p, q);
    db_load_lights(light_map, p, q);
    db_load_shapes(shape_map, p, q);
    db_load_signs(signs, p, q);
    db_load_transforms(transform_map, p, q);
}

void request_chunk(int p, int q) {
    int key = db_get_key(p, q);
    client_chunk(p, q, key);
}

void init_chunk(Chunk *chunk, int p, int q) {
    chunk->p = p;
    chunk->q = q;
    chunk->faces = 0;
    chunk->sign_faces = 0;
    chunk->buffer = 0;
    chunk->sign_buffer = 0;
    dirty_chunk(chunk);
    SignList *signs = &chunk->signs;
    sign_list_alloc(signs, 16);
    Map *block_map = &chunk->map;
    Map *extra_map = &chunk->extra;
    Map *light_map = &chunk->lights;
    Map *shape_map = &chunk->shape;
    Map *transform_map = &chunk->transform;
    DoorMap *doors_map = &chunk->doors;
    int dx = p * CHUNK_SIZE - 1;
    int dy = 0;
    int dz = q * CHUNK_SIZE - 1;
    map_alloc(block_map, dx, dy, dz, 0x3fff);
    map_alloc(extra_map, dx, dy, dz, 0xf);
    map_alloc(light_map, dx, dy, dz, 0xf);
    map_alloc(shape_map, dx, dy, dz, 0xf);
    map_alloc(transform_map, dx, dy, dz, 0xf);
    door_map_alloc(doors_map, dx, dy, dz, 0xf);
}

void create_chunk(Chunk *chunk, int p, int q) {
    init_chunk(chunk, p, q);

    WorkerItem _item;
    WorkerItem *item = &_item;
    item->p = chunk->p;
    item->q = chunk->q;
    item->block_maps[1][1] = &chunk->map;
    item->extra_maps[1][1] = &chunk->extra;
    item->light_maps[1][1] = &chunk->lights;
    item->shape_maps[1][1] = &chunk->shape;
    item->transform_maps[1][1] = &chunk->transform;
    item->door_maps[1][1] = &chunk->doors;
    load_chunk(item, g->lua_worldgen);
    sign_list_free(&chunk->signs);
    sign_list_copy(&chunk->signs, &item->signs);
    sign_list_free(&item->signs);

    request_chunk(p, q);
}

void delete_chunks(void) {
    int count = g->chunk_count;
    int states_count = 0;
    // Maximum states include the basic player view and 2 observe views.
    #define MAX_STATES (MAX_LOCAL_PLAYERS * 3)
    State *states[MAX_STATES];

    for (int p = 0; p < MAX_LOCAL_PLAYERS; p++) {
        LocalPlayer *local = g->local_players + p;
        if (local->player->is_active) {
            states[states_count++] = &local->player->state;
            if (local->observe1) {
                Client *client = find_client(local->observe1_client_id);
                if (client) {
                    Player *observe_player = client->players + local->observe1
                                             - 1;
                    states[states_count++] = &observe_player->state;
                }
            }
            if (local->observe2) {
                Client *client = find_client(local->observe2_client_id);
                if (client) {
                    Player *observe_player = client->players + local->observe2
                                             - 1;
                    states[states_count++] = &observe_player->state;
                }
            }
        }
    }

    for (int i = 0; i < count; i++) {
        Chunk *chunk = g->chunks + i;
        int delete = 1;
        for (int j = 0; j < states_count; j++) {
            State *s = states[j];
            int p = chunked(s->x);
            int q = chunked(s->z);
            if (chunk_distance(chunk, p, q) < g->delete_radius) {
                delete = 0;
                break;
            }
        }
        if (delete) {
            map_free(&chunk->map);
            map_free(&chunk->extra);
            map_free(&chunk->lights);
            map_free(&chunk->shape);
            map_free(&chunk->transform);
            sign_list_free(&chunk->signs);
            door_map_free(&chunk->doors);
            del_buffer(chunk->buffer);
            del_buffer(chunk->sign_buffer);
            Chunk *other = g->chunks + (--count);
            memcpy(chunk, other, sizeof(Chunk));
        }
    }
    g->chunk_count = count;
}

void delete_all_chunks(void) {
    for (int i = 0; i < g->chunk_count; i++) {
        Chunk *chunk = g->chunks + i;
        map_free(&chunk->map);
        map_free(&chunk->extra);
        map_free(&chunk->lights);
        map_free(&chunk->shape);
        map_free(&chunk->transform);
        door_map_free(&chunk->doors);
        sign_list_free(&chunk->signs);
        del_buffer(chunk->buffer);
        del_buffer(chunk->sign_buffer);
    }
    g->chunk_count = 0;
}

void check_workers(void) {
    for (int i = 0; i < WORKERS; i++) {
        Worker *worker = g->workers + i;
        mtx_lock(&worker->mtx);
        if (worker->state == WORKER_DONE) {
            WorkerItem *item = &worker->item;
            Chunk *chunk = find_chunk(item->p, item->q);
            if (chunk) {
                if (item->load) {
                    Map *block_map = item->block_maps[1][1];
                    Map *extra_map = item->extra_maps[1][1];
                    Map *light_map = item->light_maps[1][1];
                    Map *shape_map = item->shape_maps[1][1];
                    Map *transform_map = item->transform_maps[1][1];
                    map_free(&chunk->map);
                    map_free(&chunk->extra);
                    map_free(&chunk->lights);
                    map_free(&chunk->shape);
                    map_free(&chunk->transform);
                    sign_list_free(&chunk->signs);
                    map_copy(&chunk->map, block_map);
                    map_copy(&chunk->extra, extra_map);
                    map_copy(&chunk->lights, light_map);
                    map_copy(&chunk->shape, shape_map);
                    map_copy(&chunk->transform, transform_map);
                    sign_list_copy(&chunk->signs, &item->signs);
                    sign_list_free(&item->signs);
                    request_chunk(item->p, item->q);
                }

                // DoorMap data copy is required whether the doors were added
                // from loading game data or generated from the worldgen.
                DoorMap *door_map = item->door_maps[1][1];
                door_map_free(&chunk->doors);
                door_map_copy(&chunk->doors, door_map);

                generate_chunk(chunk, item);
            }
            for (int a = 0; a < 3; a++) {
                for (int b = 0; b < 3; b++) {
                    Map *block_map = item->block_maps[a][b];
                    Map *extra_map = item->extra_maps[a][b];
                    Map *light_map = item->light_maps[a][b];
                    Map *shape_map = item->shape_maps[a][b];
                    Map *transform_map = item->transform_maps[a][b];
                    DoorMap *door_map = item->door_maps[a][b];
                    if (block_map) {
                        map_free(block_map);
                        free(block_map);
                    }
                    if (extra_map) {
                        map_free(extra_map);
                        free(extra_map);
                    }
                    if (light_map) {
                        map_free(light_map);
                        free(light_map);
                    }
                    if (shape_map) {
                        map_free(shape_map);
                        free(shape_map);
                    }
                    if (transform_map) {
                        map_free(transform_map);
                        free(transform_map);
                    }
                    if (door_map) {
                        door_map_free(door_map);
                        free(door_map);
                    }
                }
            }
            worker->state = WORKER_IDLE;
        }
        mtx_unlock(&worker->mtx);
    }
}

void force_chunks(Player *player) {
    State *s = &player->state;
    int p = chunked(s->x);
    int q = chunked(s->z);
    int r = 1;
    for (int dp = -r; dp <= r; dp++) {
        for (int dq = -r; dq <= r; dq++) {
            int a = p + dp;
            int b = q + dq;
            Chunk *chunk = find_chunk(a, b);
            if (chunk) {
                if (chunk->dirty) {
                    gen_chunk_buffer(chunk);
                }
                if (chunk->dirty_signs) {
                    gen_sign_buffer(chunk);
                }
            }
            else if (g->chunk_count < MAX_CHUNKS) {
                chunk = g->chunks + g->chunk_count++;
                create_chunk(chunk, a, b);
                gen_chunk_buffer(chunk);
            }
        }
    }
}

void ensure_chunks_worker(Player *player, Worker *worker) {
    State *s = &player->state;
    float matrix[16];
    set_matrix_3d(
        matrix, g->width, g->height,
        s->x, s->y, s->z, s->rx, s->ry, g->fov, g->ortho, g->render_radius);
    float planes[6][4];
    frustum_planes(planes, g->render_radius, matrix);
    int p = chunked(s->x);
    int q = chunked(s->z);
    int r = g->create_radius;
    int start = 0x0fffffff;
    int best_score = start;
    int best_a = 0;
    int best_b = 0;
    for (int dp = -r; dp <= r; dp++) {
        for (int dq = -r; dq <= r; dq++) {
            int a = p + dp;
            int b = q + dq;
            int index = (ABS(a) ^ ABS(b)) % WORKERS;
            if (index != worker->index) {
                continue;
            }
            Chunk *chunk = find_chunk(a, b);
            if (chunk && !chunk->dirty) {
                continue;
            }
            int distance = MAX(ABS(dp), ABS(dq));
            int invisible = !chunk_visible(planes, a, b, 0, 256);
            int priority = 0;
            if (chunk) {
                priority = chunk->buffer && chunk->dirty;
            }
            int score = (invisible << 24) | (priority << 16) | distance;
            if (score < best_score) {
                best_score = score;
                best_a = a;
                best_b = b;
            }
        }
    }
    if (best_score == start) {
        return;
    }
    int a = best_a;
    int b = best_b;
    int load = 0;
    Chunk *chunk = find_chunk(a, b);
    if (!chunk) {
        load = 1;
        if (g->chunk_count < MAX_CHUNKS) {
            chunk = g->chunks + g->chunk_count++;
            init_chunk(chunk, a, b);
        }
        else {
            return;
        }
    }
    WorkerItem *item = &worker->item;
    item->p = chunk->p;
    item->q = chunk->q;
    item->load = load;
    for (int dp = -1; dp <= 1; dp++) {
        for (int dq = -1; dq <= 1; dq++) {
            Chunk *other = chunk;
            if (dp || dq) {
                other = find_chunk(chunk->p + dp, chunk->q + dq);
            }
            if (other) {
                Map *block_map = malloc(sizeof(Map));
                map_copy(block_map, &other->map);
                Map *extra_map = malloc(sizeof(Map));
                map_copy(extra_map, &other->extra);
                Map *light_map = malloc(sizeof(Map));
                map_copy(light_map, &other->lights);
                Map *shape_map = malloc(sizeof(Map));
                map_copy(shape_map, &other->shape);
                Map *transform_map = malloc(sizeof(Map));
                map_copy(transform_map, &other->transform);
                DoorMap *door_map = malloc(sizeof(DoorMap));
                door_map_copy(door_map, &other->doors);
                item->block_maps[dp + 1][dq + 1] = block_map;
                item->extra_maps[dp + 1][dq + 1] = extra_map;
                item->light_maps[dp + 1][dq + 1] = light_map;
                item->shape_maps[dp + 1][dq + 1] = shape_map;
                item->transform_maps[dp + 1][dq + 1] = transform_map;
                item->door_maps[dp + 1][dq + 1] = door_map;
            }
            else {
                item->block_maps[dp + 1][dq + 1] = 0;
                item->extra_maps[dp + 1][dq + 1] = 0;
                item->light_maps[dp + 1][dq + 1] = 0;
                item->shape_maps[dp + 1][dq + 1] = 0;
                item->transform_maps[dp + 1][dq + 1] = 0;
                item->door_maps[dp + 1][dq + 1] = 0;
            }
        }
    }
    chunk->dirty = 0;
    worker->state = WORKER_BUSY;
    cnd_signal(&worker->cnd);
}

void ensure_chunks(Player *player) {
    check_workers();
    force_chunks(player);
    for (int i = 0; i < WORKERS; i++) {
        Worker *worker = g->workers + i;
        mtx_lock(&worker->mtx);
        if (worker->state == WORKER_IDLE) {
            ensure_chunks_worker(player, worker);
        }
        mtx_unlock(&worker->mtx);
    }
}

int worker_run(void *arg) {
    Worker *worker = (Worker *)arg;
    lua_State *L = NULL;
    if (g->use_lua_worldgen == 1) {
        L = pwlua_worldgen_init(config->worldgen_path);
    }
    while (!worker->exit_requested) {
        mtx_lock(&worker->mtx);
        while (worker->state != WORKER_BUSY) {
            cnd_wait(&worker->cnd, &worker->mtx);
            if (worker->exit_requested) {
                if (L != NULL) {
                    lua_close(L);
                }
                thrd_exit(1);
            }
        }
        mtx_unlock(&worker->mtx);
        WorkerItem *item = &worker->item;
        if (item->load) {
            load_chunk(item, L);
        }
        compute_chunk(item);
        mtx_lock(&worker->mtx);
        worker->state = WORKER_DONE;
        mtx_unlock(&worker->mtx);
    }
    if (L != NULL) {
        lua_close(L);
    }
    return 0;
}

void unset_sign(int x, int y, int z) {
    int p = chunked(x);
    int q = chunked(z);
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        SignList *signs = &chunk->signs;
        if (sign_list_remove_all(signs, x, y, z)) {
            chunk->dirty_signs = 1;
            db_delete_signs(x, y, z);
        }
    }
    else {
        db_delete_signs(x, y, z);
    }
}

void unset_sign_face(int x, int y, int z, int face) {
    int p = chunked(x);
    int q = chunked(z);
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        SignList *signs = &chunk->signs;
        if (sign_list_remove(signs, x, y, z, face)) {
            chunk->dirty_signs = 1;
            db_delete_sign(x, y, z, face);
        }
    }
    else {
        db_delete_sign(x, y, z, face);
    }
}

void _set_sign(
    int p, int q, int x, int y, int z, int face, const char *text, int dirty)
{
    if (strlen(text) == 0) {
        unset_sign_face(x, y, z, face);
        return;
    }
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        SignList *signs = &chunk->signs;
        sign_list_add(signs, x, y, z, face, text);
        if (dirty) {
            chunk->dirty_signs = 1;
        }
    }
    db_insert_sign(p, q, x, y, z, face, text);
}

void set_sign(int x, int y, int z, int face, const char *text) {
    int p = chunked(x);
    int q = chunked(z);
    _set_sign(p, q, x, y, z, face, text, 1);
    client_sign(x, y, z, face, text);
}

void worldgen_set_sign(int x, int y, int z, int face, const char *text,
                       SignList *sign_list)
{
    sign_list_add(sign_list, x, y, z, face, text);
}

const unsigned char *get_sign(int p, int q, int x, int y, int z, int face) {
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        SignList *signs = &chunk->signs;
        for (size_t i = 0; i < signs->size; i++) {
            Sign *e = signs->data + i;
            if (e->x == x && e->y == y && e->z == z && e->face == face) {
                return (const unsigned char *)e->text;
            }
        }
    } else {
        // TODO: support server
        return db_get_sign(p, q, x, y, z, face);
    }
    return NULL;
}

void toggle_light(int x, int y, int z) {
    int p = chunked(x);
    int q = chunked(z);
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        Map *map = &chunk->lights;
        int w = map_get(map, x, y, z) ? 0 : 15;
        map_set(map, x, y, z, w);
        db_insert_light(p, q, x, y, z, w);
        client_light(x, y, z, w);
        dirty_chunk(chunk);
    }
}

int _set_light(int p, int q, int x, int y, int z, int w) {
    Chunk *chunk = find_chunk(p, q);
    if (w < 0) {
        w = 0;
    }
    if (w > 15) {
        w = 15;
    }
    if (chunk) {
        Map *map = &chunk->lights;
        if (map_set(map, x, y, z, w)) {
            dirty_chunk(chunk);
            db_insert_light(p, q, x, y, z, w);
        }
    }
    else {
        db_insert_light(p, q, x, y, z, w);
    }
    return w;
}

void set_light(int p, int q, int x, int y, int z, int w) {
    w = _set_light(p, q, x, y, z, w);
    client_light(x, y, z, w);
}

int get_light(int p, int q, int x, int y, int z) {
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        Map *map = &chunk->lights;
        return map_get(map, x, y, z);
    } else {
        // TODO: support server
        if (g->chunk_count < MAX_CHUNKS) {
            chunk = g->chunks + g->chunk_count++;
            create_chunk(chunk, p, q);
            return map_get(&chunk->lights, x, y, z);
        }
    }
    return 0;
}

void set_extra(int x, int y, int z, int w) {
    int p = chunked(x);
    int q = chunked(z);
    _set_extra(p, q, x, y, z, w, 1);
    for (int dx = -1; dx <= 1; dx++) {
        for (int dz = -1; dz <= 1; dz++) {
            if (dx == 0 && dz == 0) {
                continue;
            }
            if (dx && chunked(x + dx) == p) {
                continue;
            }
            if (dz && chunked(z + dz) == q) {
                continue;
            }
            _set_extra(p + dx, q + dz, x, y, z, -w, 1);
        }
    }
    client_extra(x, y, z, w);
}

void set_extra_non_dirty(int x, int y, int z, int w) {
    int p = chunked(x);
    int q = chunked(z);
    _set_extra(p, q, x, y, z, w, 0);
    for (int dx = -1; dx <= 1; dx++) {
        for (int dz = -1; dz <= 1; dz++) {
            if (dx == 0 && dz == 0) {
                continue;
            }
            if (dx && chunked(x + dx) == p) {
                continue;
            }
            if (dz && chunked(z + dz) == q) {
                continue;
            }
            _set_extra(p + dx, q + dz, x, y, z, -w, 0);
        }
    }
    client_extra(x, y, z, w);
}

void _set_extra(int p, int q, int x, int y, int z, int w, int dirty) {
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        Map *map = &chunk->extra;
        if (map_set(map, x, y, z, w)) {
            if (dirty) {
                dirty_chunk(chunk);
            }
            db_insert_extra(p, q, x, y, z, w);
        }
    }
    else {
        db_insert_extra(p, q, x, y, z, w);
    }
}

int get_extra(int x, int y, int z) {
    int p = chunked(x);
    int q = chunked(z);
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        Map *map = &chunk->extra;
        return map_get(map, x, y, z);
    } else {
        // TODO: support server
        if (g->chunk_count < MAX_CHUNKS) {
            chunk = g->chunks + g->chunk_count++;
            create_chunk(chunk, p, q);
            return map_get(&chunk->extra, x, y, z);
        }
    }
    return 0;
}

void set_shape(int x, int y, int z, int w) {
    int p = chunked(x);
    int q = chunked(z);
    _set_shape(p, q, x, y, z, w, 1);
    for (int dx = -1; dx <= 1; dx++) {
        for (int dz = -1; dz <= 1; dz++) {
            if (dx == 0 && dz == 0) {
                continue;
            }
            if (dx && chunked(x + dx) == p) {
                continue;
            }
            if (dz && chunked(z + dz) == q) {
                continue;
            }
            _set_shape(p + dx, q + dz, x, y, z, -w, 1);
        }
    }
    client_shape(x, y, z, w);
}

void _set_shape(int p, int q, int x, int y, int z, int w, int dirty) {
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        Map *map = &chunk->shape;
        if (map_set(map, x, y, z, w)) {
            if (dirty) {
                dirty_chunk(chunk);
            }
            db_insert_shape(p, q, x, y, z, w);
        }
    }
    else {
        db_insert_shape(p, q, x, y, z, w);
    }
}

int get_shape(int x, int y, int z) {
    int p = chunked(x);
    int q = chunked(z);
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        Map *map = &chunk->shape;
        return map_get(map, x, y, z);
    } else {
        // TODO: support server
        if (g->chunk_count < MAX_CHUNKS) {
            chunk = g->chunks + g->chunk_count++;
            create_chunk(chunk, p, q);
            return map_get(&chunk->shape, x, y, z);
        }
    }
    return 0;
}

void set_transform(int x, int y, int z, int w) {
    int p = chunked(x);
    int q = chunked(z);
    _set_transform(p, q, x, y, z, w, 1);
    for (int dx = -1; dx <= 1; dx++) {
        for (int dz = -1; dz <= 1; dz++) {
            if (dx == 0 && dz == 0) {
                continue;
            }
            if (dx && chunked(x + dx) == p) {
                continue;
            }
            if (dz && chunked(z + dz) == q) {
                continue;
            }
            _set_transform(p + dx, q + dz, x, y, z, -w, 1);
        }
    }
    client_transform(x, y, z, w);
}

void _set_transform(int p, int q, int x, int y, int z, int w, int dirty) {
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        Map *map = &chunk->transform;
        if (map_set(map, x, y, z, w)) {
            if (dirty) {
                dirty_chunk(chunk);
            }
            db_insert_transform(p, q, x, y, z, w);
        }
    }
    else {
        db_insert_transform(p, q, x, y, z, w);
    }
}

int get_transform(int x, int y, int z) {
    int p = chunked(x);
    int q = chunked(z);
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        Map *map = &chunk->transform;
        return map_get(map, x, y, z);
    } else {
        // TODO: support server
        if (g->chunk_count < MAX_CHUNKS) {
            chunk = g->chunks + g->chunk_count++;
            create_chunk(chunk, p, q);
            return map_get(&chunk->transform, x, y, z);
        }
    }
    return 0;
}

void _set_block(int p, int q, int x, int y, int z, int w, int dirty) {
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        Map *map = &chunk->map;
        if (map_set(map, x, y, z, w)) {
            if (dirty) {
                dirty_chunk(chunk);
            }
            db_insert_block(p, q, x, y, z, w);
        }
    }
    else {
        db_insert_block(p, q, x, y, z, w);
    }
    if (w == 0 && chunked(x) == p && chunked(z) == q) {
        unset_sign(x, y, z);
        _set_light(p, q, x, y, z, 0);
        _set_extra(p, q, x, y, z, 0, 1);
        _set_shape(p, q, x, y, z, 0, 1);
        _set_transform(p, q, x, y, z, 0, 1);
        door_map_clear(&chunk->doors, x, y, z);
    }
}

void set_block(int x, int y, int z, int w) {
    int p = chunked(x);
    int q = chunked(z);
    _set_block(p, q, x, y, z, w, 1);
    for (int dx = -1; dx <= 1; dx++) {
        for (int dz = -1; dz <= 1; dz++) {
            if (dx == 0 && dz == 0) {
                continue;
            }
            if (dx && chunked(x + dx) == p) {
                continue;
            }
            if (dz && chunked(z + dz) == q) {
                continue;
            }
            _set_block(p + dx, q + dz, x, y, z, -w, 1);
        }
    }
    client_block(x, y, z, w);
}

static int file_readable(const char *filename)
{
    FILE *f = fopen(filename, "r");  /* try to open file */
    if (f == NULL) return 0;  /* open failed */
    fclose(f);
    return 1;
}

void set_worldgen(char *worldgen)
{
    if (g->use_lua_worldgen) {
        lua_close(g->lua_worldgen);
        g->lua_worldgen = NULL;
        config->worldgen_path[0] = '\0';
        g->use_lua_worldgen = 0;
    }
    if (worldgen && strlen(worldgen) > 0) {
        char wg_path[MAX_PATH_LENGTH];
        if (!file_readable(worldgen)) {
            snprintf(wg_path, MAX_PATH_LENGTH, "%s/worldgen/%s.lua",
                     get_data_dir(), worldgen);
        } else {
            snprintf(wg_path, MAX_PATH_LENGTH, "%s", worldgen);
        }
        if (!file_readable(wg_path)) {
            printf("Worldgen file not found: %s\n", wg_path);
            return;
        }
        strncpy(config->worldgen_path, wg_path, sizeof(config->worldgen_path));
        config->worldgen_path[sizeof(config->worldgen_path)-1] = '\0';
        g->lua_worldgen = pwlua_worldgen_init(config->worldgen_path);
        g->use_lua_worldgen = 1;
    }
    g->render_option_changed = 1;
}

void queue_set_block(int x, int y, int z, int w) {
    int p = chunked(x);
    int q = chunked(z);
    mtx_lock(&edit_ring_mtx);
    ring_put_block(&g->edit_ring, p, q, x, y, z, w);
    mtx_unlock(&edit_ring_mtx);
}

void queue_set_extra(int x, int y, int z, int w) {
    int p = chunked(x);
    int q = chunked(z);
    mtx_lock(&edit_ring_mtx);
    ring_put_extra(&g->edit_ring, p, q, x, y, z, w);
    mtx_unlock(&edit_ring_mtx);
}

void queue_set_light(int x, int y, int z, int w) {
    int p = chunked(x);
    int q = chunked(z);
    mtx_lock(&edit_ring_mtx);
    ring_put_light(&g->edit_ring, p, q, x, y, z, w);
    mtx_unlock(&edit_ring_mtx);
}

void queue_set_shape(int x, int y, int z, int w) {
    int p = chunked(x);
    int q = chunked(z);
    mtx_lock(&edit_ring_mtx);
    ring_put_shape(&g->edit_ring, p, q, x, y, z, w);
    mtx_unlock(&edit_ring_mtx);
}

void queue_set_sign(int x, int y, int z, int face, const char *text) {
    int p = chunked(x);
    int q = chunked(z);
    mtx_lock(&edit_ring_mtx);
    ring_put_sign(&g->edit_ring, p, q, x, y, z, face, text);
    mtx_unlock(&edit_ring_mtx);
}

void queue_set_transform(int x, int y, int z, int w) {
    int p = chunked(x);
    int q = chunked(z);
    mtx_lock(&edit_ring_mtx);
    ring_put_transform(&g->edit_ring, p, q, x, y, z, w);
    mtx_unlock(&edit_ring_mtx);
}

void record_block(int x, int y, int z, int w, LocalPlayer *local) {
    memcpy(&local->block1, &local->block0, sizeof(Block));
    local->block0.x = x;
    local->block0.y = y;
    local->block0.z = z;
    local->block0.w = w;
}

int get_block(int x, int y, int z) {
    int p = chunked(x);
    int q = chunked(z);
    Chunk *chunk = find_chunk(p, q);
    if (chunk) {
        Map *map = &chunk->map;
        return map_get(map, x, y, z);
    } else {
        // TODO: support server
        if (g->chunk_count < MAX_CHUNKS) {
            chunk = g->chunks + g->chunk_count++;
            create_chunk(chunk, p, q);
            return map_get(&chunk->map, x, y, z);
        }
    }
    return 0;
}

void builder_block(int x, int y, int z, int w) {
    if (y <= 0 || y >= 256) {
        return;
    }
    if (is_destructable(get_block(x, y, z))) {
        set_block(x, y, z, 0);
    }
    if (w) {
        set_block(x, y, z, w);
    }
}

int render_chunks(Attrib *attrib, Player *player) {
    int result = 0;
    State *s = &player->state;
    ensure_chunks(player);
    int p = chunked(s->x);
    int q = chunked(s->z);
    float light = get_daylight();
    float matrix[16];
    set_matrix_3d(
        matrix, g->width, g->height,
        s->x, s->y, s->z, s->rx, s->ry, g->fov, g->ortho, g->render_radius);
    float planes[6][4];
    frustum_planes(planes, g->render_radius, matrix);
    glUseProgram(attrib->program);
    glUniformMatrix4fv(attrib->matrix, 1, GL_FALSE, matrix);
    glUniform3f(attrib->camera, s->x, s->y, s->z);
    glUniform1i(attrib->sampler, 0);
    glUniform1i(attrib->extra1, 2);
    glUniform1f(attrib->extra2, light);
    glUniform1f(attrib->extra3, g->render_radius * CHUNK_SIZE);
    glUniform1i(attrib->extra4, g->ortho);
    glUniform1f(attrib->timer, time_of_day());
    for (int i = 0; i < g->chunk_count; i++) {
        Chunk *chunk = g->chunks + i;
        if (chunk_distance(chunk, p, q) > g->render_radius) {
            continue;
        }
        if (!chunk_visible(
            planes, chunk->p, chunk->q, chunk->miny, chunk->maxy))
        {
            continue;
        }
        glUniform4f(attrib->map, chunk->map.dx, chunk->map.dy, chunk->map.dz, 0);
        draw_chunk(attrib, chunk);
        result += chunk->faces;
    }
    return result;
}

void render_signs(Attrib *attrib, Player *player) {
    State *s = &player->state;
    int p = chunked(s->x);
    int q = chunked(s->z);
    float matrix[16];
    set_matrix_3d(
        matrix, g->width, g->height,
        s->x, s->y, s->z, s->rx, s->ry, g->fov, g->ortho, g->render_radius);
    float planes[6][4];
    frustum_planes(planes, g->render_radius, matrix);
    glUseProgram(attrib->program);
    glUniformMatrix4fv(attrib->matrix, 1, GL_FALSE, matrix);
    glUniform3f(attrib->camera, s->x, s->y, s->z);
    glUniform1i(attrib->sampler, 3);
    glUniform1i(attrib->extra1, 1);  // is_sign
    glUniform1i(attrib->extra2, 2);  // sky_sampler
    glUniform1f(attrib->extra3, g->render_radius * CHUNK_SIZE); // fog_distance
    glUniform1i(attrib->extra4, g->ortho);  // ortho
    glUniform1f(attrib->timer, time_of_day());
    for (int i = 0; i < g->chunk_count; i++) {
        Chunk *chunk = g->chunks + i;
        if (chunk_distance(chunk, p, q) > g->sign_radius) {
            continue;
        }
        if (!chunk_visible(
            planes, chunk->p, chunk->q, chunk->miny, chunk->maxy))
        {
            continue;
        }
        draw_signs(attrib, chunk);
    }
}

void render_sign(Attrib *attrib, LocalPlayer *local) {
    if (!local->typing || local->typing_buffer[0] != CRAFT_KEY_SIGN) {
        return;
    }
    int x, y, z, face;
    if (!hit_test_face(local->player, &x, &y, &z, &face)) {
        return;
    }
    State *s = &local->player->state;
    float matrix[16];
    set_matrix_3d(
        matrix, g->width, g->height,
        s->x, s->y, s->z, s->rx, s->ry, g->fov, g->ortho, g->render_radius);
    glUseProgram(attrib->program);
    glUniformMatrix4fv(attrib->matrix, 1, GL_FALSE, matrix);
    glUniform1i(attrib->sampler, 3);
    glUniform1i(attrib->extra1, 1);
    char text[MAX_SIGN_LENGTH];
    strncpy(text, local->typing_buffer + 1, MAX_SIGN_LENGTH);
    text[MAX_SIGN_LENGTH - 1] = '\0';
    GLfloat *data = malloc_faces_with_rgba(5, strlen(text));
    int length = _gen_sign_buffer(data, x, y, z, face, text);
    GLuint buffer = gen_faces_with_rgba(5, length, data);
    draw_sign(attrib, buffer, length);
    del_buffer(buffer);
}

void render_players(Attrib *attrib, Player *player) {
    State *s = &player->state;
    float matrix[16];
    set_matrix_3d(
        matrix, g->width, g->height,
        s->x, s->y, s->z, s->rx, s->ry, g->fov, g->ortho, g->render_radius);
    glUseProgram(attrib->program);
    glUniformMatrix4fv(attrib->matrix, 1, GL_FALSE, matrix);
    glUniform3f(attrib->camera, s->x, s->y, s->z);
    glUniform1i(attrib->sampler, 0);
    glUniform1f(attrib->timer, time_of_day());
    for (int i = 0; i < g->client_count; i++) {
        Client *client = g->clients + i;
        for (int j = 0; j < MAX_LOCAL_PLAYERS; j++) {
            Player *other = client->players + j;
            if (other != player && other->is_active) {
                glUniform4f(attrib->map, 0, 0, 0, 0);
                draw_player(attrib, other);
            }
        }
    }
}

void render_sky(Attrib *attrib, Player *player, GLuint buffer) {
    State *s = &player->state;
    float matrix[16];
    set_matrix_3d(
        matrix, g->width, g->height,
        0, 0, 0, s->rx, s->ry, g->fov, 0, g->render_radius);
    glUseProgram(attrib->program);
    glUniformMatrix4fv(attrib->matrix, 1, GL_FALSE, matrix);
    glUniform1i(attrib->sampler, 2);
    glUniform1f(attrib->timer, time_of_day());
    draw_triangles_3d_sky(attrib, buffer, 128 * 3);
}

void render_wireframe(Attrib *attrib, Player *player) {
    State *s = &player->state;
    float matrix[16];
    set_matrix_3d(
        matrix, g->width, g->height,
        s->x, s->y, s->z, s->rx, s->ry, g->fov, g->ortho, g->render_radius);
    int hx, hy, hz;
    int hw = hit_test(0, s->x, s->y, s->z, s->rx, s->ry, &hx, &hy, &hz);
    if (is_obstacle(hw, 0, 0)) {
        glUseProgram(attrib->program);
        glLineWidth(1);
        glUniformMatrix4fv(attrib->matrix, 1, GL_FALSE, matrix);
        if (is_control(get_extra(hx, hy, hz))) {
            glUniform4fv(attrib->extra1, 1, RED);
        } else {
            glUniform4fv(attrib->extra1, 1, BLACK);
        }
        int shape = get_shape(hx, hy, hz);
        float h = item_height(shape) - 0.97;
        GLuint wireframe_buffer = gen_wireframe_buffer(hx, hy, hz, 0.53, h);
        draw_lines(attrib, wireframe_buffer, 3, 24);
        del_buffer(wireframe_buffer);
    }
}

void render_crosshairs(Attrib *attrib, Player *player) {
    State *s = &player->state;
    float matrix[16];
    set_matrix_2d(matrix, g->width, g->height);
    glUseProgram(attrib->program);
    glLineWidth(4 * g->scale);
    glUniformMatrix4fv(attrib->matrix, 1, GL_FALSE, matrix);
    glUniform4fv(attrib->extra1, 1, BLACK);
    int hx, hy, hz;
    int hw = hit_test(0, s->x, s->y, s->z, s->rx, s->ry, &hx, &hy, &hz);
    if (is_obstacle(hw, 0, 0) && is_control(get_extra(hx, hy, hz))) {
        glUniform4fv(attrib->extra1, 1, RED);
    }
    GLuint crosshair_buffer = gen_crosshair_buffer();
    draw_lines(attrib, crosshair_buffer, 2, 4);
    del_buffer(crosshair_buffer);
}

void render_item(Attrib *attrib, LocalPlayer *p) {
    float matrix[16];
    set_matrix_item(matrix, g->width, g->height, g->scale);
    glUseProgram(attrib->program);
    glUniformMatrix4fv(attrib->matrix, 1, GL_FALSE, matrix);
    glUniform3f(attrib->camera, 0, 0, 5);
    glUniform1i(attrib->sampler, 0);
    glUniform1f(attrib->timer, time_of_day());
    glUniform4f(attrib->map, 0, 0, 0, 0);
    int w = items[p->item_index];
    if (is_plant(w)) {
        GLuint buffer = gen_plant_buffer(0, 0, 0, 0.5, w);
        draw_plant(attrib, buffer);
        del_buffer(buffer);
    }
    else {
        GLuint buffer = gen_cube_buffer(0, 0, 0, 0.5, w);
        draw_cube(attrib, buffer, g->float_size, g->gl_float_type);
        del_buffer(buffer);
    }
}

void render_text_rgba(
    Attrib *attrib, int justify, float x, float y, float n, char *text,
    const float *background, const float *text_color)
{
    float matrix[16];
    set_matrix_2d(matrix, g->width, g->height);
    glUseProgram(attrib->program);
    glUniformMatrix4fv(attrib->matrix, 1, GL_FALSE, matrix);
    glUniform1i(attrib->sampler, 3);
    glUniform1i(attrib->extra1, 0);
    glUniform4fv(attrib->extra5, 1, background);
    glUniform4fv(attrib->extra6, 1, text_color);
    int length = strlen(text);
    x -= n * justify * (length - 1) / 2;
    GLuint buffer = gen_text_buffer(x, y, n, text);
    draw_text(attrib, buffer, length);
    del_buffer(buffer);
}

void render_text(
    Attrib *attrib, int justify, float x, float y, float n, char *text)
{
    render_text_rgba(attrib, justify, x, y, n, text, hud_text_background,
                     hud_text_color);
}

GLuint gen_text_cursor_buffer(float x, float y) {
    int p = 10 * g->scale;
    float data[] = {
        x, y - p, x, y + p,
    };
    return gen_buffer(sizeof(data), data);
}

void render_text_cursor(Attrib *attrib, float x, float y)
{
    float matrix[16];
    set_matrix_2d(matrix, g->width, g->height);
    glUseProgram(attrib->program);
    glLineWidth(2 * g->scale);
    glUniformMatrix4fv(attrib->matrix, 1, GL_FALSE, matrix);
    glUniform4fv(attrib->extra1, 1, GREEN);
    GLuint text_cursor_buffer = gen_text_cursor_buffer(x, y);
    draw_lines(attrib, text_cursor_buffer, 2, 2);
    del_buffer(text_cursor_buffer);
}

void render_mouse_cursor(Attrib *attrib, float x, float y, int p)
{
    float matrix[16];
    set_matrix_2d(matrix, g->width, g->height);
    glUseProgram(attrib->program);
    glUniformMatrix4fv(attrib->matrix, 1, GL_FALSE, matrix);
    glUniform1i(attrib->sampler, 0);
    GLuint mouse_cursor_buffer = gen_mouse_cursor_buffer(x, y, p);
    draw_mouse(attrib, mouse_cursor_buffer);
    del_buffer(mouse_cursor_buffer);
}

void add_message(int player_id, const char *text) {
    if (player_id < 1 || player_id > MAX_LOCAL_PLAYERS) {
        printf("Message for invalid player %d: %s\n", player_id, text);
        return;
    }
    LocalPlayer *local = g->local_players + player_id - 1;
    printf("%d: %s\n", player_id, text);
    snprintf(
        local->messages[local->message_index], MAX_TEXT_LENGTH, "%s", text);
    local->message_index = (local->message_index + 1) % MAX_MESSAGES;
}

void login(void) {
    printf("Logging in anonymously\n");
    client_login("", "");
}

void copy(LocalPlayer *local) {
    memcpy(&local->copy0, &local->block0, sizeof(Block));
    memcpy(&local->copy1, &local->block1, sizeof(Block));
}

void paste(LocalPlayer *local) {
    Block *c1 = &local->copy1;
    Block *c2 = &local->copy0;
    Block *p1 = &local->block1;
    Block *p2 = &local->block0;
    int scx = SIGN(c2->x - c1->x);
    int scz = SIGN(c2->z - c1->z);
    int spx = SIGN(p2->x - p1->x);
    int spz = SIGN(p2->z - p1->z);
    int oy = p1->y - c1->y;
    int dx = ABS(c2->x - c1->x);
    int dz = ABS(c2->z - c1->z);
    for (int y = 0; y < 256; y++) {
        for (int x = 0; x <= dx; x++) {
            for (int z = 0; z <= dz; z++) {
                int w = get_block(c1->x + x * scx, y, c1->z + z * scz);
                builder_block(p1->x + x * spx, y + oy, p1->z + z * spz, w);
            }
        }
    }
}

void array(Block *b1, Block *b2, int xc, int yc, int zc) {
    if (b1->w != b2->w) {
        return;
    }
    int w = b1->w;
    int dx = b2->x - b1->x;
    int dy = b2->y - b1->y;
    int dz = b2->z - b1->z;
    xc = dx ? xc : 1;
    yc = dy ? yc : 1;
    zc = dz ? zc : 1;
    for (int i = 0; i < xc; i++) {
        int x = b1->x + dx * i;
        for (int j = 0; j < yc; j++) {
            int y = b1->y + dy * j;
            for (int k = 0; k < zc; k++) {
                int z = b1->z + dz * k;
                builder_block(x, y, z, w);
            }
        }
    }
}

void cube(Block *b1, Block *b2, int fill) {
    if (b1->w != b2->w) {
        return;
    }
    int w = b1->w;
    int x1 = MIN(b1->x, b2->x);
    int y1 = MIN(b1->y, b2->y);
    int z1 = MIN(b1->z, b2->z);
    int x2 = MAX(b1->x, b2->x);
    int y2 = MAX(b1->y, b2->y);
    int z2 = MAX(b1->z, b2->z);
    int a = (x1 == x2) + (y1 == y2) + (z1 == z2);
    for (int x = x1; x <= x2; x++) {
        for (int y = y1; y <= y2; y++) {
            for (int z = z1; z <= z2; z++) {
                if (!fill) {
                    int n = 0;
                    n += x == x1 || x == x2;
                    n += y == y1 || y == y2;
                    n += z == z1 || z == z2;
                    if (n <= a) {
                        continue;
                    }
                }
                builder_block(x, y, z, w);
            }
        }
    }
}

void fence(Block *b1, Block *b2) {
    if (b1->w != b2->w) {
        return;
    }
    int w = b1->w;
    int x1 = MIN(b1->x, b2->x);
    int y1 = MIN(b1->y, b2->y);
    int z1 = MIN(b1->z, b2->z);
    int x2 = MAX(b1->x, b2->x);
    int y2 = MAX(b1->y, b2->y);
    int z2 = MAX(b1->z, b2->z);
    int a = (x1 == x2) + (y1 == y2) + (z1 == z2);
    for (int x = x1; x <= x2; x++) {
        for (int y = y1; y <= y2; y++) {
            for (int z = z1; z <= z2; z++) {
                int n = 0;
                n += x == x1 || x == x2;
                n += y == y1 || y == y2;
                n += z == z1 || z == z2;
                if (n <= a) {
                    continue;
                }
                if (x > x1 && x < x2 && z > z1 && z < z2) {
                    continue;
                }
                builder_block(x, y, z, w);
                int shape = FENCE;
                if (x == x1 && z == z1) {
                    shape = FENCE_L;  // corner
                    set_transform(x, y, z, 2);
                } else if (x == x2 && z == z1) {
                    shape = FENCE_L;  // corner
                } else if (x == x2 && z == z2) {
                    shape = FENCE_L;  // corner
                    set_transform(x, y, z, 1);
                } else if (x == x1 && z == z2) {
                    shape = FENCE_L;  // corner
                    set_transform(x, y, z, 3);
                } else if (x != x1 && x != x2) {
                    // other two sides of the fence
                    set_transform(x, y, z, 1);
                }
                set_shape(x, y, z, shape);
            }
        }
    }
}

void sphere(Block *center, int radius, int fill, int fx, int fy, int fz) {
    static const float offsets[8][3] = {
        {-0.5, -0.5, -0.5},
        {-0.5, -0.5, 0.5},
        {-0.5, 0.5, -0.5},
        {-0.5, 0.5, 0.5},
        {0.5, -0.5, -0.5},
        {0.5, -0.5, 0.5},
        {0.5, 0.5, -0.5},
        {0.5, 0.5, 0.5}
    };
    int cx = center->x;
    int cy = center->y;
    int cz = center->z;
    int w = center->w;
    for (int x = cx - radius; x <= cx + radius; x++) {
        if (fx && x != cx) {
            continue;
        }
        for (int y = cy - radius; y <= cy + radius; y++) {
            if (fy && y != cy) {
                continue;
            }
            for (int z = cz - radius; z <= cz + radius; z++) {
                if (fz && z != cz) {
                    continue;
                }
                int inside = 0;
                int outside = fill;
                for (int i = 0; i < 8; i++) {
                    float dx = x + offsets[i][0] - cx;
                    float dy = y + offsets[i][1] - cy;
                    float dz = z + offsets[i][2] - cz;
                    float d = sqrtf(dx * dx + dy * dy + dz * dz);
                    if (d < radius) {
                        inside = 1;
                    }
                    else {
                        outside = 1;
                    }
                }
                if (inside && outside) {
                    builder_block(x, y, z, w);
                }
            }
        }
    }
}

void cylinder(Block *b1, Block *b2, int radius, int fill) {
    if (b1->w != b2->w) {
        return;
    }
    int w = b1->w;
    int x1 = MIN(b1->x, b2->x);
    int y1 = MIN(b1->y, b2->y);
    int z1 = MIN(b1->z, b2->z);
    int x2 = MAX(b1->x, b2->x);
    int y2 = MAX(b1->y, b2->y);
    int z2 = MAX(b1->z, b2->z);
    int fx = x1 != x2;
    int fy = y1 != y2;
    int fz = z1 != z2;
    if (fx + fy + fz != 1) {
        return;
    }
    Block block = {x1, y1, z1, w};
    if (fx) {
        for (int x = x1; x <= x2; x++) {
            block.x = x;
            sphere(&block, radius, fill, 1, 0, 0);
        }
    }
    if (fy) {
        for (int y = y1; y <= y2; y++) {
            block.y = y;
            sphere(&block, radius, fill, 0, 1, 0);
        }
    }
    if (fz) {
        for (int z = z1; z <= z2; z++) {
            block.z = z;
            sphere(&block, radius, fill, 0, 0, 1);
        }
    }
}

void tree(Block *block) {
    int bx = block->x;
    int by = block->y;
    int bz = block->z;
    for (int y = by + 3; y < by + 8; y++) {
        for (int dx = -3; dx <= 3; dx++) {
            for (int dz = -3; dz <= 3; dz++) {
                int dy = y - (by + 4);
                int d = (dx * dx) + (dy * dy) + (dz * dz);
                if (d < 11) {
                    builder_block(bx + dx, y, bz + dz, 15);
                }
            }
        }
    }
    for (int y = by; y < by + 7; y++) {
        builder_block(bx, y, bz, 5);
    }
}

void parse_command(LocalPlayer *local, const char *buffer, int forward) {
    char server_addr[MAX_ADDR_LENGTH];
    int server_port = DEFAULT_PORT;
    char filename[MAX_FILENAME_LENGTH];
    char name[MAX_NAME_LENGTH];
    int int_option, radius, count, p, q, xc, yc, zc;
    char window_title[MAX_TITLE_LENGTH];
    char worldgen_path[MAX_PATH_LENGTH];
    Player *player = local->player;
    if (strcmp(buffer, "/fullscreen") == 0) {
        pg_toggle_fullscreen();
    }
    else if (sscanf(buffer,
        "/online %128s %d", server_addr, &server_port) >= 1)
    {
        g->mode_changed = 1;
        g->mode = MODE_ONLINE;
        strncpy(config->server, server_addr, MAX_ADDR_LENGTH);
        config->port = server_port;
        get_server_db_cache_path(g->db_path);
    }
    else if (sscanf(buffer, "/offline %128s", filename) == 1) {
        g->mode_changed = 1;
        g->mode = MODE_OFFLINE;
        snprintf(g->db_path, MAX_PATH_LENGTH, "%s/%s.piworld", config->path,
                 filename);
    }
    else if (strcmp(buffer, "/offline") == 0) {
        g->mode_changed = 1;
        g->mode = MODE_OFFLINE;
        get_default_db_path(g->db_path);
    }
    else if (sscanf(buffer, "/nick %32c", name) == 1) {
        int prefix_length = strlen("/nick ");
        name[MIN(strlen(buffer) - prefix_length, MAX_NAME_LENGTH-1)] = '\0';
        strncpy(player->name, name, MAX_NAME_LENGTH);
        client_nick(player->id, name);
    }
    else if (strcmp(buffer, "/spawn") == 0) {
        client_spawn(player->id);
    }
    else if (strcmp(buffer, "/goto") == 0) {
        client_goto(player->id, "");
    }
    else if (sscanf(buffer, "/goto %32c", name) == 1) {
        client_goto(player->id, name);
    }
    else if (sscanf(buffer, "/pq %d %d", &p, &q) == 2) {
        client_pq(player->id, p, q);
    }
    else if (sscanf(buffer, "/view %d", &radius) == 1) {
        if (radius >= 0 && radius <= 24) {
            set_view_radius(radius, g->delete_radius);
        }
        else {
            add_message(player->id,
                        "Viewing distance must be between 0 and 24.");
        }
    }
    else if (sscanf(buffer, "/players %d", &int_option) == 1) {
        if (int_option >= 1 && int_option <= MAX_LOCAL_PLAYERS) {
            g->auto_add_players_on_new_devices = 0;
            if (config->players != int_option) {
                config->players = int_option;
                limit_player_count_to_fit_gpu_mem();
                set_player_count(g->clients, config->players);
                set_view_radius(g->render_radius, g->delete_radius);
            }
        }
        else {
            add_message(player->id, "Player count must be between 1 and 4.");
        }
    }
    else if (strcmp(buffer, "/exit") == 0) {
        if (config->players <= 1) {
            terminate = True;
        } else {
            client_remove_player(player->id);
            player->is_active = 0;
            g->auto_add_players_on_new_devices = 0;
            config->players -= 1;
            local->keyboard_id = UNASSIGNED;
            local->mouse_id = UNASSIGNED;
            local->joystick_id = UNASSIGNED;
            set_players_view_size(g->width, g->height);
            set_view_radius(g->render_radius, g->delete_radius);
        }
    }
    else if (sscanf(buffer, "/position %d %d %d", &xc, &yc, &zc) == 3) {
        State *s = &player->state;
        s->x = xc;
        s->y = yc;
        s->z = zc;
    }
    else if (sscanf(buffer, "/show-chat-text %d", &int_option) == 1) {
        config->show_chat_text = int_option;
    }
    else if (sscanf(buffer, "/show-clouds %d", &int_option) == 1) {
        if (g->mode == MODE_OFFLINE) {
            char value[2];
            snprintf(value, 2, "%d", int_option);
            db_set_option("show-clouds", value);
            config->show_clouds = int_option;
            g->render_option_changed = 1;  // regenerate world
        } else {
            printf("Cannot change worldgen when connected to server.\n");
        }
    }
    else if (sscanf(buffer, "/show-crosshairs %d", &int_option) == 1) {
        config->show_crosshairs = int_option;
    }
    else if (sscanf(buffer, "/show-item %d", &int_option) == 1) {
        config->show_item = int_option;
    }
    else if (sscanf(buffer, "/show-info-text %d", &int_option) == 1) {
        config->show_info_text = int_option;
    }
    else if (sscanf(buffer, "/show-lights %d", &int_option) == 1) {
        if (g->mode == MODE_OFFLINE) {
            config->show_lights = int_option;
            g->render_option_changed = 1;  // regenerate world
        } else {
            printf("Cannot change worldgen when connected to server.\n");
        }
    }
    else if (sscanf(buffer, "/show-plants %d", &int_option) == 1) {
        if (g->mode == MODE_OFFLINE) {
            char value[2];
            snprintf(value, 2, "%d", int_option);
            db_set_option("show-plants", value);
            config->show_plants = int_option;
            g->render_option_changed = 1;  // regenerate world
        } else {
            printf("Cannot change worldgen when connected to server.\n");
        }
    }
    else if (sscanf(buffer, "/show-player-names %d", &int_option) == 1) {
        config->show_player_names = int_option;
    }
    else if (sscanf(buffer, "/show-trees %d", &int_option) == 1) {
        if (g->mode == MODE_OFFLINE) {
            char value[2];
            snprintf(value, 2, "%d", int_option);
            db_set_option("show-trees", value);
            config->show_trees = int_option;
            g->render_option_changed = 1;  // regenerate world
        } else {
            printf("Cannot change worldgen when connected to server.\n");
        }
    }
    else if (strcmp(buffer, "/worldgen") == 0) {
        if (g->mode == MODE_OFFLINE) {
            set_worldgen(NULL);
            db_set_option("worldgen", "");
        } else {
            printf("Cannot change worldgen when connected to server.\n");
        }
    }
    else if (sscanf(buffer, "/worldgen %512c", worldgen_path) == 1) {
        if (g->mode == MODE_OFFLINE) {
            int prefix_length = strlen("/worldgen ");;
            worldgen_path[strlen(buffer) - prefix_length] = '\0';
            set_worldgen(worldgen_path);
            db_set_option("worldgen", worldgen_path);
        } else {
            printf("Cannot change worldgen when connected to server.\n");
        }
    }
    else if (sscanf(buffer, "/show-wireframe %d", &int_option) == 1) {
        config->show_wireframe = int_option;
    }
    else if (sscanf(buffer, "/verbose %d", &int_option) == 1) {
        config->verbose = int_option;
    }
    else if (sscanf(buffer, "/vsync %d", &int_option) == 1) {
        config->vsync = int_option;
        pg_swap_interval(config->vsync);
    }
    else if (sscanf(buffer, "/window-size %d %d", &xc, &yc) == 2) {
        pg_resize_window(xc, yc);
    }
    else if (sscanf(buffer, "/window-title %128c", window_title) == 1) {
        int prefix_length = strlen("/window-title ");;
        window_title[strlen(buffer) - prefix_length] = '\0';
        pg_set_window_title(window_title);
    }
    else if (sscanf(buffer, "/window-xy %d %d", &xc, &yc) == 2) {
        pg_move_window(xc, yc);
    }
    else if (strcmp(buffer, "/copy") == 0) {
        copy(local);
    }
    else if (strcmp(buffer, "/paste") == 0) {
        paste(local);
    }
    else if (strcmp(buffer, "/tree") == 0) {
        tree(&local->block0);
    }
    else if (sscanf(buffer, "/array %d %d %d", &xc, &yc, &zc) == 3) {
        array(&local->block1, &local->block0, xc, yc, zc);
    }
    else if (sscanf(buffer, "/array %d", &count) == 1) {
        array(&local->block1, &local->block0, count, count, count);
    }
    else if (strcmp(buffer, "/fcube") == 0) {
        cube(&local->block0, &local->block1, 1);
    }
    else if (strcmp(buffer, "/cube") == 0) {
        cube(&local->block0, &local->block1, 0);
    }
    else if (sscanf(buffer, "/fsphere %d", &radius) == 1) {
        sphere(&local->block0, radius, 1, 0, 0, 0);
    }
    else if (sscanf(buffer, "/sphere %d", &radius) == 1) {
        sphere(&local->block0, radius, 0, 0, 0, 0);
    }
    else if (sscanf(buffer, "/fcirclex %d", &radius) == 1) {
        sphere(&local->block0, radius, 1, 1, 0, 0);
    }
    else if (sscanf(buffer, "/circlex %d", &radius) == 1) {
        sphere(&local->block0, radius, 0, 1, 0, 0);
    }
    else if (sscanf(buffer, "/fcircley %d", &radius) == 1) {
        sphere(&local->block0, radius, 1, 0, 1, 0);
    }
    else if (sscanf(buffer, "/circley %d", &radius) == 1) {
        sphere(&local->block0, radius, 0, 0, 1, 0);
    }
    else if (sscanf(buffer, "/fcirclez %d", &radius) == 1) {
        sphere(&local->block0, radius, 1, 0, 0, 1);
    }
    else if (sscanf(buffer, "/circlez %d", &radius) == 1) {
        sphere(&local->block0, radius, 0, 0, 0, 1);
    }
    else if (sscanf(buffer, "/fcylinder %d", &radius) == 1) {
        cylinder(&local->block0, &local->block1, radius, 1);
    }
    else if (sscanf(buffer, "/cylinder %d", &radius) == 1) {
        cylinder(&local->block0, &local->block1, radius, 0);
    }
    else if (strcmp(buffer, "/fence") == 0) {
        fence(&local->block0, &local->block1);
    }
    else if (sscanf(buffer, "/delete-radius %d", &radius) == 1) {
        if (radius >= g->create_radius && radius <= g->create_radius + 30) {
            set_view_radius(g->render_radius, radius);
        }
    }
    else if (sscanf(buffer, "/time %d", &int_option) == 1) {
        if (g->mode == MODE_OFFLINE && int_option >= 0 && int_option <= 24) {
            pg_set_time(g->day_length /
                        (24.0 / (int_option == 0 ? 24 : int_option)));
            g->time_changed = 1;
        }
    }
    else if (forward) {
        client_talk(buffer);
    }
}

void clear_block_under_crosshair(LocalPlayer *local)
{
    State *s = &local->player->state;
    int hx, hy, hz;
    int hw = hit_test(0, s->x, s->y, s->z, s->rx, s->ry, &hx, &hy, &hz);
    if (hy > 0 && hy < 256 && is_destructable(hw)) {
        // If control block then run callback and do not remove.
        if (is_control(get_extra(hx, hy, hz))) {
            int face;
            hit_test_face(local->player, &hx, &hy, &hz, &face);
            client_control_callback(local->player->id, hx, hy, hz, face);
            pwlua_control_callback(local->player->id, hx, hy, hz, face);
            return;
        }

        // Save the block to be removed.
        int p = chunked(hx);
        int q = chunked(hz);
        Chunk *chunk = find_chunk(p, q);
        local->undo_block.x = hx;
        local->undo_block.y = hy;
        local->undo_block.z = hz;
        local->undo_block.texture = hw;
        local->undo_block.extra = get_extra(hx, hy, hz);
        local->undo_block.light = get_light(p, q, hx, hy, hz);
        local->undo_block.shape = get_shape(hx, hy, hz);
        local->undo_block.transform = get_transform(hx, hy, hz);
        if (local->undo_block.has_sign) {
            sign_list_free(&local->undo_block.signs);
        }
        local->undo_block.has_sign = 0;
        SignList *undo_signs = &local->undo_block.signs;
        for (size_t i = 0; i < chunk->signs.size; i++) {
            Sign *e = chunk->signs.data + i;
            if (e->x == hx && e->y == hy && e->z == hz) {
                if (local->undo_block.has_sign == 0) {
                    sign_list_alloc(undo_signs, 1);
                    local->undo_block.has_sign = 1;
                }
                sign_list_add(undo_signs, hx, hy, hz, e->face, e->text);
            }
        }
        local->has_undo_block = 1;

        set_block(hx, hy, hz, 0);
        record_block(hx, hy, hz, 0, local);
        if (config->verbose) {
            printf("%s cleared: x: %d y: %d z: %d w: %d\n",
                   local->player->name, hx, hy, hz, hw);
        }
        int hw_above = get_block(hx, hy + 1, hz);
        if (is_plant(hw_above)) {
            set_block(hx, hy + 1, hz, 0);
            if (config->verbose) {
                printf("%s cleared: x: %d y: %d z: %d w: %d\n",
                       local->player->name, hx, hy + 1, hz, hw_above);
            }
        }
    }
}

void set_block_under_crosshair(LocalPlayer *local)
{
    State *s = &local->player->state;
    int hx, hy, hz;
    int hw = hit_test(1, s->x, s->y, s->z, s->rx, s->ry, &hx, &hy, &hz);
    int hx2, hy2, hz2;
    int hw2 = hit_test(0, s->x, s->y, s->z, s->rx, s->ry, &hx2, &hy2, &hz2);
    if (hy2 > 0 && hy2 < 256 && is_obstacle(hw2, 0, 0)) {
        int shape = get_shape(hx2, hy2, hz2);
        int extra = get_extra(hx2, hy2, hz2);
        if (shape == LOWER_DOOR || shape == UPPER_DOOR) {
            // toggle open/close
            int p = chunked(hx2);
            int q = chunked(hz2);
            Chunk *chunk = find_chunk(p, q);
            DoorMapEntry *door = door_map_get(&chunk->doors, hx2, hy2, hz2);
            door_toggle_open(&chunk->doors, door, hx2, hy2, hz2, chunk->buffer, g->float_size);
            return;
        } else if (is_control(extra)) {
            open_menu(local, &local->menu);
            return;
        } else if (shape == GATE) {
            // toggle open/close
            int p = chunked(hx2);
            int q = chunked(hz2);
            Chunk *chunk = find_chunk(p, q);
            DoorMapEntry *gate = door_map_get(&chunk->doors, hx2, hy2, hz2);
            gate_toggle_open(gate, hx2, hy2, hz2, chunk->buffer, g->float_size);
            return;
        }
    }
    if (hy > 0 && hy < 256 && is_obstacle(hw, 0, 0)) {
        if (!player_intersects_block(2, s->x, s->y, s->z, hx, hy, hz)) {
            set_block(hx, hy, hz, items[local->item_index]);
            record_block(hx, hy, hz, items[local->item_index], local);
        }
        if (config->verbose) {
            printf("%s set: x: %d y: %d z: %d w: %d\n",
                   local->player->name, hx, hy, hz, items[local->item_index]);
        }
    }
}

void set_item_in_hand_to_item_under_crosshair(LocalPlayer *local)
{
    State *s = &local->player->state;
    int i = 0;
    int hx, hy, hz;
    int hw = hit_test(0, s->x, s->y, s->z, s->rx, s->ry, &hx, &hy, &hz);
    if (hw > 0) {
        for (i = 0; i < item_count; i++) {
            if (items[i] == hw) {
                local->item_index = i;
                break;
            }
        }
        if (config->verbose) {
            printf("%s selected: x: %d y: %d z: %d w: %d\n",
                   local->player->name, hx, hy, hz, hw);
        }
    }
}

void open_menu_for_item_under_crosshair(LocalPlayer *local)
{
    State *s = &local->player->state;
    int hx, hy, hz;
    int hw = hit_test(0, s->x, s->y, s->z, s->rx, s->ry, &hx, &hy, &hz);
    if (hw == 0) {
        open_menu(local, &local->menu_item_in_hand);
    } else {
        int p, q, x, y, z, face;
        char *sign_text;
        int i = 0;
        for (i = 0; i < item_count; i++) {
            if (items[i] == hw) {
                break;
            }
        }
        hit_test_face(local->player, &x, &y, &z, &face);
        p = chunked(x);
        q = chunked(z);
        sign_text = (char *)get_sign(p, q, x, y, z, face);
        local->edit_x = x;
        local->edit_y = y;
        local->edit_z = z;
        local->edit_face = face;
        populate_block_edit_menu(local, i + 1, sign_text,
                                 get_light(p, q, x, y, z), get_extra(x, y, z),
                                 get_shape(x, y, z), get_transform(x, y, z));
        open_menu(local, &local->menu_block_edit);
    }
}

int pw_get_crosshair(int player_id, int *x, int *y, int *z, int *face)
{
    if (player_id < 1 || player_id > MAX_LOCAL_PLAYERS) {
        return 0;
    }
    LocalPlayer *local = &g->local_players[player_id - 1];
    int hw = hit_test_face(local->player, x, y, z, face);
    if (hw > 0) {
        return 1;
    }
    return 0;
}

void cycle_item_in_hand_up(LocalPlayer *player) {
    player->item_index = (player->item_index + 1) % item_count;
}

void cycle_item_in_hand_down(LocalPlayer *player) {
    player->item_index--;
    if (player->item_index < 0) {
        player->item_index = item_count - 1;
    }
}

void on_light(LocalPlayer *local) {
    State *s = &local->player->state;
    int hx, hy, hz;
    int hw = hit_test(0, s->x, s->y, s->z, s->rx, s->ry, &hx, &hy, &hz);
    if (hy > 0 && hy < 256 && is_destructable(hw)) {
        toggle_light(hx, hy, hz);
    }
}

void handle_mouse_motion(int mouse_id, float x, float y) {
    LocalPlayer *local = player_for_mouse(mouse_id);
    if (local->active_menu) {
        local->mouse_x -= x;
        local->mouse_y += y;
        if (local->mouse_x < 0) {
            local->mouse_x = 0;
        }
        if (local->mouse_x > local->view_width - 1) {
            local->mouse_x = local->view_width - 1;
        }
        if (local->mouse_y < -(MOUSE_CURSOR_SIZE - 1)) {
            local->mouse_y = -(MOUSE_CURSOR_SIZE - 1);
        }
        if (local->mouse_y > local->view_height - MOUSE_CURSOR_SIZE) {
            local->mouse_y = local->view_height - MOUSE_CURSOR_SIZE;
        }
        menu_handle_mouse(local->active_menu, local->mouse_x, local->mouse_y);
        return;
    }
    State *s = &local->player->state;
    float m = 0.0025;
    if (local->zoom_is_pressed) {
        m = 0.0005;
    }
    s->rx -= x * m;
    s->ry += y * m;
    if (s->rx < 0) {
        s->rx += RADIANS(360);
    }
    if (s->rx >= RADIANS(360)) {
        s->rx -= RADIANS(360);
    }
    s->ry = MAX(s->ry, -RADIANS(90));
    s->ry = MIN(s->ry, RADIANS(90));
}

void handle_movement(double dt, LocalPlayer *local) {
    State *s = &local->player->state;
    int stay_in_crouch = 0;
    float sz = 0;
    float sx = 0;
    if (!local->typing) {
        float m1 = dt * local->view_speed_left_right;
        float m2 = dt * local->view_speed_up_down;

        // Walking
        if (local->forward_is_pressed) sz = -local->movement_speed_forward_back;
        if (local->back_is_pressed) sz = local->movement_speed_forward_back;
        if (local->left_is_pressed) sx = -local->movement_speed_left_right;
        if (local->right_is_pressed) sx = local->movement_speed_left_right;

        // View direction
        if (local->view_left_is_pressed) s->rx -= m1;
        if (local->view_right_is_pressed) s->rx += m1;
        if (local->view_up_is_pressed) s->ry += m2;
        if (local->view_down_is_pressed) s->ry -= m2;
    }
    float vx, vy, vz;
    get_motion_vector(local->flying, sz, sx, s->rx, s->ry, &vx, &vy, &vz);
    if (!local->typing) {
        if (local->jump_is_pressed) {
            if (local->flying) {
                vy = 1;
            }
            else if (local->dy == 0) {
                local->dy = 8;
            }
        } else if (local->crouch_is_pressed) {
            if (local->flying) {
                vy = -1;
            }
            else if (local->dy == 0) {
                local->dy = -4;
            }
        } else {
            // If previously in a crouch, move to standing position
            int block_under_player_head = get_block(
                roundf(s->x), s->y, roundf(s->z));
            int block_above_player_head = get_block(
                roundf(s->x), s->y + 2, roundf(s->z));
            int shape = get_shape(roundf(s->x), s->y, roundf(s->z));
            int extra = get_extra(roundf(s->x), s->y, roundf(s->z));
            if (is_obstacle(block_under_player_head, shape, extra)) {
                shape = get_shape(roundf(s->x), s->y + 2, roundf(s->z));
                extra = get_extra(roundf(s->x), s->y + 2, roundf(s->z));
                if (is_obstacle(block_above_player_head, shape, extra)) {
                    stay_in_crouch = 1;
                } else {
                    local->dy = 8;
                }
            }
        }
    }
    float speed = 5;  // walking speed
    if (local->flying) {
        speed = 20;
    } else if (local->crouch_is_pressed || stay_in_crouch) {
        speed = 2;
    }
    int estimate = roundf(sqrtf(
        powf(vx * speed, 2) +
        powf(vy * speed + ABS(local->dy) * 2, 2) +
        powf(vz * speed, 2)) * dt * 8);
    int step = MAX(8, estimate);
    float ut = dt / step;
    vx = vx * ut * speed;
    vy = vy * ut * speed;
    vz = vz * ut * speed;
    for (int i = 0; i < step; i++) {
        if (local->flying) {
            local->dy = 0;
        }
        else {
            local->dy -= ut * 25;
            local->dy = MAX(local->dy, -250);
        }
        s->x += vx;
        s->y += vy + local->dy * ut;
        s->z += vz;
        int player_standing_height = 2;
        int player_couching_height = 1;
        int player_min_height = player_standing_height;
        if (local->crouch_is_pressed || stay_in_crouch) {
            player_min_height = player_couching_height;
        }
        if (collide(player_min_height, &s->x, &s->y, &s->z, &local->dy)) {
            local->dy = 0;
        }
    }
    if (s->y < 0) {
        s->y = highest_block(s->x, s->z) + 2;
    }
}

void parse_buffer(char *buffer) {
    #define INVALID_PLAYER_INDEX (p < 1 || p > MAX_LOCAL_PLAYERS)
    Client *local_client = g->clients;
    char *key;
    char *line = tokenize(buffer, "\n", &key);
    while (line) {
        int pid;
        int p;
        float px, py, pz, prx, pry;
        if (sscanf(line, "P,%d,%d,%f,%f,%f,%f,%f",
            &pid, &p, &px, &py, &pz, &prx, &pry) == 7)
        {
            if (INVALID_PLAYER_INDEX) goto next_line;
            Client *client = find_client(pid);
            if (!client && g->client_count < MAX_CLIENTS) {
                // Add a new client
                client = g->clients + g->client_count;
                g->client_count++;
                client->id = pid;
                // Initialize the players.
                for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
                    Player *player = client->players + i;
                    player->is_active = 0;
                    player->id = i + 1;
                    player->buffer = 0;
                    player->texture_index = i;
                }
            }
            if (client) {
                Player *player = &client->players[p - 1];
                if (!player->is_active) {
                    // Add remote player
                    player->is_active = 1;
                    snprintf(player->name, MAX_NAME_LENGTH, "player%d-%d",
                             pid, p);
                    update_player(player, px, py, pz, prx, pry, 1);
                    client_add_player(player->id);
                } else {
                    update_player(player, px, py, pz, prx, pry, 1);
                }
            }
            goto next_line;
        }
        float ux, uy, uz, urx, ury;
        if (sscanf(line, "U,%d,%d,%f,%f,%f,%f,%f",
            &pid, &p, &ux, &uy, &uz, &urx, &ury) == 7)
        {
            if (INVALID_PLAYER_INDEX) goto next_line;
            Player *me = local_client->players + (p-1);
            State *s = &me->state;
            local_client->id = pid;
            s->x = ux; s->y = uy; s->z = uz; s->rx = urx; s->ry = ury;
            force_chunks(me);
            if (uy == 0) {
                s->y = highest_block(s->x, s->z) + 2;
            }
            goto next_line;
        }

        int bp, bq, bx, by, bz, bw;
        if (sscanf(line, "B,%d,%d,%d,%d,%d,%d",
            &bp, &bq, &bx, &by, &bz, &bw) == 6)
        {
            Player *me = local_client->players;
            State *s = &me->state;
            _set_block(bp, bq, bx, by, bz, bw, 0);
            if (player_intersects_block(2, s->x, s->y, s->z, bx, by, bz)) {
                s->y = highest_block(s->x, s->z) + 2;
            }
            goto next_line;
        }
        if (sscanf(line, "e,%d,%d,%d,%d,%d,%d",
            &bp, &bq, &bx, &by, &bz, &bw) == 6)
        {
            _set_extra(bp, bq, bx, by, bz, bw, 0);
            goto next_line;
        }
        if (sscanf(line, "s,%d,%d,%d,%d,%d,%d",
            &bp, &bq, &bx, &by, &bz, &bw) == 6)
        {
            _set_shape(bp, bq, bx, by, bz, bw, 0);
            goto next_line;
        }
        if (sscanf(line, "t,%d,%d,%d,%d,%d,%d",
            &bp, &bq, &bx, &by, &bz, &bw) == 6)
        {
            _set_transform(bp, bq, bx, by, bz, bw, 0);
            goto next_line;
        }
        if (sscanf(line, "L,%d,%d,%d,%d,%d,%d",
            &bp, &bq, &bx, &by, &bz, &bw) == 6)
        {
            _set_light(bp, bq, bx, by, bz, bw);
            goto next_line;
        }
        if (sscanf(line, "X,%d,%d", &pid, &p) == 2) {
            if (INVALID_PLAYER_INDEX) goto next_line;
            Client *client = find_client(pid);
            if (client) {
                Player *player = &client->players[p - 1];
                player->is_active = 0;
            }
            goto next_line;
        }
        if (sscanf(line, "D,%d", &pid) == 1) {
            delete_client(pid);
            goto next_line;
        }
        int kp, kq, kk;
        if (sscanf(line, "K,%d,%d,%d", &kp, &kq, &kk) == 3) {
            db_set_key(kp, kq, kk);
            goto next_line;
        }
        if (sscanf(line, "R,%d,%d", &kp, &kq) == 2) {
            Chunk *chunk = find_chunk(kp, kq);
            if (chunk) {
                dirty_chunk(chunk);
            }
            goto next_line;
        }
        double elapsed;
        int day_length;
        if (sscanf(line, "E,%lf,%d", &elapsed, &day_length) == 2) {
            pg_set_time(fmod(elapsed, day_length));
            g->day_length = day_length;
            g->time_changed = 1;
            goto next_line;
        }
        if (line[0] == 'T' && line[1] == ',') {
            char *text = line + 2;
            for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
                add_message(i+1, text);
            }
            goto next_line;
        }
        char format[64];
        snprintf(
            format, sizeof(format), "N,%%d,%%d,%%%ds", MAX_NAME_LENGTH - 1);
        char name[MAX_NAME_LENGTH];
        if (sscanf(line, format, &pid, &p, name) == 3) {
            if (INVALID_PLAYER_INDEX) goto next_line;
            Client *client = find_client(pid);
            if (client) {
                strncpy(client->players[p - 1].name, name, MAX_NAME_LENGTH);
            }
            goto next_line;
        }
        char value[MAX_NAME_LENGTH];
        snprintf(
            format, sizeof(format), "O,%%%d[^,],%%%d[^,]", MAX_NAME_LENGTH - 1,
            MAX_NAME_LENGTH - 1);
        if (sscanf(line, format, name, value) == 2) {
            printf("Got option from server %s = %s\n", name, value);
            int int_value = atoi(value);
            if (strncmp(name, "show-plants", 11) == 0 &&
                (int_value == 0 || int_value == 1)) {
                if (int_value != config->show_plants) {
                    config->show_plants = int_value;
                    g->render_option_changed = 1;  // regenerate world
                }
            } else if (strncmp(name, "show-trees", 9) == 0 &&
                       (int_value == 0 || int_value == 1)) {
                if (int_value != config->show_trees) {
                    config->show_trees = int_value;
                    g->render_option_changed = 1;  // regenerate world
                }
            } else if (strncmp(name, "show-clouds", 11) == 0 &&
                       (int_value == 0 || int_value == 1)) {
                if (int_value != config->show_clouds) {
                    config->show_clouds = int_value;
                    g->render_option_changed = 1;  // regenerate world
                }
            } else if (strncmp(name, "worldgen", 8) == 0) {
                // Only except a named worldgen or empty for default.
                // Only a worldgen script under the client's ./worldgen dir
                // will be excepted.
                if (strlen(value) > 0) {
                    if (strchr(value, '/')) {
                        printf(
                "Path component not allowed in worldgen from server: %s\n"
                "Please ask the server admin to use named worldgens only.\n",
                               value);
                        goto next_line;
                    }
                    set_worldgen(value);
                } else {
                    set_worldgen(NULL);
                }
            }
            goto next_line;
        }
        snprintf(
            format, sizeof(format),
            "S,%%d,%%d,%%d,%%d,%%d,%%d,%%%d[^\n]", MAX_SIGN_LENGTH - 1);
        int face;
        char text[MAX_SIGN_LENGTH] = {0};
        if (sscanf(line, format,
            &bp, &bq, &bx, &by, &bz, &face, text) >= 6)
        {
            _set_sign(bp, bq, bx, by, bz, face, text, 0);
            goto next_line;
        }

next_line:
        line = tokenize(NULL, "\n", &key);
    }
}

void create_menus(LocalPlayer *local)
{
    Menu *menu;

    // Main menu
    menu = &local->menu;
    menu_set_title(menu, "PIWORLD");
    local->menu_id_resume = menu_add(menu, "Resume");
    local->menu_id_options = menu_add(menu, "Options");
    local->menu_id_new = menu_add(menu, "New");
    local->menu_id_load = menu_add(menu, "Load");
    local->menu_id_exit = menu_add(menu, "Exit");

    // Options menu
    menu = &local->menu_options;
    menu_set_title(menu, "OPTIONS");
    local->menu_id_script = menu_add(menu, "Script");
    local->menu_id_crosshairs = menu_add_option(menu, "Crosshairs");
    local->menu_id_fullscreen = menu_add_option(menu, "Fullscreen");
    local->menu_id_verbose = menu_add_option(menu, "Verbose");
    local->menu_id_wireframe = menu_add_option(menu, "Wireframe");
    local->menu_id_worldgen = menu_add(menu, "Worldgen");
    local->menu_id_options_resume = menu_add(menu, "Resume");
    menu_set_option(menu, local->menu_id_crosshairs, config->show_crosshairs);
    menu_set_option(menu, local->menu_id_fullscreen, config->fullscreen);
    menu_set_option(menu, local->menu_id_verbose, config->verbose);
    menu_set_option(menu, local->menu_id_wireframe, config->show_wireframe);

    // New menu
    menu = &local->menu_new;
    menu_set_title(menu, "NEW GAME");
    local->menu_id_new_game_name = menu_add_line_edit(menu, "Name");
    local->menu_id_new_ok = menu_add(menu, "OK");
    local->menu_id_new_cancel = menu_add(menu, "Cancel");

    // Load menu
    menu = &local->menu_load;
    menu_set_title(menu, "LOAD GAME");

    // Item in hand menu
    menu = &local->menu_item_in_hand;
    menu_set_title(menu, "ITEM IN HAND");
    populate_texture_menu(menu);
    local->menu_id_item_in_hand_cancel = menu_add(menu, "Cancel");

    // Block edit menu
    menu = &local->menu_block_edit;
    menu_set_title(menu, "EDIT BLOCK");

    // Texture menu
    menu = &local->menu_texture;
    menu_set_title(menu, "TEXTURE");
    populate_texture_menu(menu);
    local->menu_id_texture_cancel = menu_add(menu, "Cancel");

    // Shape menu
    menu = &local->menu_shape;
    menu_set_title(menu, "SHAPE");
    populate_shape_menu(menu);
    local->menu_id_shape_cancel = menu_add(menu, "Cancel");

    // Script menu
    menu = &local->menu_script;
    menu_set_title(menu, "SCRIPT");
    local->menu_id_script_run = menu_add(menu, "Run");
    local->menu_id_script_cancel = menu_add(menu, "Cancel");

    // Run script menu
    menu = &local->menu_script_run;
    char *path = realpath(".", NULL);
    snprintf(local->menu_script_run_dir, MAX_DIR_LENGTH, path);
    free(path);
    menu_set_title(menu, "RUN SCRIPT");

    // Worldgen menu
    menu = &local->menu_worldgen;
    menu_set_title(menu, "WORLDGEN");
    local->menu_id_worldgen_select = menu_add(menu, "Select");
    local->menu_id_worldgen_cancel = menu_add(menu, "Cancel");

    // Worldgen select menu
    menu = &local->menu_worldgen_select;
    snprintf(local->menu_worldgen_dir, MAX_DIR_LENGTH, "%s/worldgen",
             get_data_dir());
    menu_set_title(menu, "SELECT WORLDGEN");
}

void populate_block_edit_menu(LocalPlayer *local, int w, char *sign_text,
    int light, int extra, int shape, int transform)
{
    Menu *menu = &local->menu_block_edit;
    menu_clear_items(menu);
    char text[MAX_TEXT_LENGTH];
    snprintf(text, MAX_TEXT_LENGTH, "Texture: %s", item_names[w - 1]);
    local->menu_id_texture = menu_add(menu, text);
    local->menu_id_sign_text = menu_add_line_edit(menu, "Sign Text");
    if (sign_text) {
        menu_set_text(menu, local->menu_id_sign_text, sign_text);
    }
    local->menu_id_light = menu_add_line_edit(menu, "Light (0-15)");
    if (light) {
        char light_text[MAX_TEXT_LENGTH];
        snprintf(light_text, MAX_TEXT_LENGTH, "%d", light);
        menu_set_text(menu, local->menu_id_light, light_text);
    }
    snprintf(text, MAX_TEXT_LENGTH, "Shape: %s", shape_names[shape]);
    local->menu_id_shape = menu_add(menu, text);
    local->menu_id_transform = menu_add_line_edit(menu, "Transform (0-7)");
    if (transform) {
        char transform_text[MAX_TEXT_LENGTH];
        snprintf(transform_text, MAX_TEXT_LENGTH, "%d", transform);
        menu_set_text(menu, local->menu_id_transform, transform_text);
    }
    local->menu_id_control_bit = menu_add_option(menu, "Control Bit");
    menu_set_option(menu, local->menu_id_control_bit, is_control(extra));
    local->menu_id_open_bit = menu_add_option(menu, "Open");
    menu_set_option(menu, local->menu_id_open_bit, is_open(extra));
    local->menu_id_block_edit_resume = menu_add(menu, "Resume");
}

void populate_texture_menu(Menu *menu)
{
    for (int i=0; i<item_count; i++) {
        menu_add(menu, (char *)item_names[i]);
    }
}

void populate_shape_menu(Menu *menu)
{
    for (int i=0; i<shape_count; i++) {
        menu_add(menu, (char *)shape_names[i]);
    }
}

void populate_load_menu(LocalPlayer *local)
{
    Menu *menu = &local->menu_load;
    menu_clear_items(menu);
    DIR *dir = opendir(config->path);
    struct dirent *dp;
    for (;;) {
        char world_name[MAX_TEXT_LENGTH];
        dp = readdir(dir);
        if (dp == NULL) {
            break;
        }
        if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0 ||
            strlen(dp->d_name) <= 8 ||
            strcmp(dp->d_name + (strlen(dp->d_name) - 8), ".piworld") != 0) {
            continue;
        }
        snprintf(world_name, strlen(dp->d_name) - 8 + 1, "%s", dp->d_name);
        menu_add(menu, world_name);
    }
    closedir(dir);
    menu_sort(menu);
    local->menu_id_load_cancel = menu_add(menu, "Cancel");
}

void populate_script_run_menu(LocalPlayer *local)
{
    Menu *menu = &local->menu_script_run;
    menu_clear_items(menu);
    menu_add(menu, "..");
    DIR *dir = opendir(local->menu_script_run_dir);
    struct dirent *dp;
    for (;;) {
        dp = readdir(dir);
        if (dp == NULL) {
            break;
        }
        if (dp->d_name[0] == '.') {
            continue;  // ignore hidden files
        }
        char path[MAX_PATH_LENGTH];
        snprintf(path, MAX_PATH_LENGTH, "%s/%s", local->menu_script_run_dir,
                 dp->d_name);
        struct stat sb;
        stat(path, &sb);
        if (S_ISDIR(sb.st_mode)) {
            snprintf(path, MAX_PATH_LENGTH, "%s/", dp->d_name);
        } else {
            if (strlen(dp->d_name) <= 4 ||
                strcmp(dp->d_name + (strlen(dp->d_name) - 4), ".lua") != 0) {
                continue;
            }
            snprintf(path, MAX_PATH_LENGTH, "%s", dp->d_name);
        }
        menu_add(menu, path);
    }
    closedir(dir);
    menu_sort(menu);
    local->menu_id_script_run_cancel = menu_add(menu, "Cancel");
}

void populate_worldgen_select_menu(LocalPlayer *local)
{
    Menu *menu = &local->menu_worldgen_select;
    menu_clear_items(menu);
    DIR *dir = opendir(local->menu_worldgen_dir);
    struct dirent *dp;
    for (;;) {
        dp = readdir(dir);
        if (dp == NULL) {
            break;
        }
        if (dp->d_name[0] == '.') {
            continue;  // ignore hidden files
        }
        char path[MAX_PATH_LENGTH];
        char wg[MAX_PATH_LENGTH];
        snprintf(path, MAX_PATH_LENGTH, "%s/%s", local->menu_worldgen_dir,
                 dp->d_name);
        struct stat sb;
        stat(path, &sb);
        if (S_ISDIR(sb.st_mode)) {
            continue;  // ignore directories
        } else {
            if (strlen(dp->d_name) <= 4 ||
                strcmp(dp->d_name + (strlen(dp->d_name) - 4), ".lua") != 0) {
                continue;
            }
            snprintf(wg, MAX_PATH_LENGTH, "%s", dp->d_name);
            *strrchr(wg, '.') = '\0';
        }
        menu_add(menu, wg);
    }
    closedir(dir);
    menu_sort(menu);
    local->menu_id_worldgen_select_cancel = menu_add(menu, "Cancel");
}

void reset_model(void) {
    memset(g->chunks, 0, sizeof(Chunk) * MAX_CHUNKS);
    g->chunk_count = 0;
    memset(g->clients, 0, sizeof(Client) * MAX_CLIENTS);
    g->client_count = 0;
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = &g->local_players[i];
        local->flying = 0;
        local->item_index = 0;
        local->typing = 0;
        local->observe1 = 0;
        local->observe1_client_id = 0;
        local->observe2 = 0;
        local->observe2_client_id = 0;
        memset(local->typing_buffer, 0, sizeof(char) * MAX_TEXT_LENGTH);
        memset(local->messages, 0,
               sizeof(char) * MAX_MESSAGES * MAX_TEXT_LENGTH);
        local->message_index = 0;
        local->has_undo_block = 0;
        local->undo_block.has_sign = 0;
    }
    g->day_length = DAY_LENGTH;
    const unsigned char *stored_time;
    stored_time = db_get_option("time");
    if (config->time >= 0 && config->time <= 24) {
        pg_set_time(g->day_length /
                    (24.0 / (config->time == 0 ? 24 : config->time)));
    } else if (stored_time != NULL) {
        pg_set_time(g->day_length * atof((char *)stored_time));
    } else {
        pg_set_time(g->day_length / 3.0);
    }
    g->time_changed = 1;
    set_player_count(g->clients, config->players);
    ring_empty(&g->edit_ring);
    ring_alloc(&g->edit_ring, 1024);
}

void insert_into_typing_buffer(LocalPlayer *local, unsigned char c) {
    size_t n = strlen(local->typing_buffer);
    if (n < MAX_TEXT_LENGTH - 1) {
        if (local->text_cursor != n) {
            // Shift text after the text cursor to the right
            memmove(local->typing_buffer + local->text_cursor + 1,
                    local->typing_buffer + local->text_cursor,
                    n - local->text_cursor);
        }
        local->typing_buffer[local->text_cursor] = c;
        local->typing_buffer[n + 1] = '\0';
        local->text_cursor += 1;
    }
}

void history_add(TextLineHistory *history, char *line)
{
    if (MAX_HISTORY_SIZE == 0 || strlen(line) <= history->line_start) {
        // Ignore empty lines
        return;
    }

    int duplicate_line = 1;
    if (history->size == 0 ||
        strcmp(line, history->lines[history->end]) != 0) {
        duplicate_line = 0;
    }
    if (!duplicate_line) {
        // Add a non-duplicate line to the history
        if (history->size >= MAX_HISTORY_SIZE) {
            memmove(history->lines, history->lines + 1,
                    (MAX_HISTORY_SIZE-1) * MAX_TEXT_LENGTH);
        }
        history->size = MIN(history->size + 1, MAX_HISTORY_SIZE);
        snprintf(history->lines[history->size - 1], MAX_TEXT_LENGTH, "%s",
                 line);
        history->end = history->size - 1;
    }
}

void history_previous(TextLineHistory *history, char *line, int *position)
{
    if (history->size == 0) {
        return;
    }

    int new_position;
    if (*position == NOT_IN_HISTORY) {
        new_position = history->end;
    } else if (*position == 0) {
        new_position = history->size - 1;
    } else {
        new_position = (*position - 1) % history->size;
    }

    if (*position != NOT_IN_HISTORY && new_position == history->end) {
        // Stop if already at start of history
        return;
    }

    *position = new_position;
    snprintf(line, MAX_TEXT_LENGTH, "%s", history->lines[*position]);
}

void history_next(TextLineHistory *history, char *line, int *position)
{
    if (history->size == 0 || *position == NOT_IN_HISTORY ) {
        return;
    }

    int new_position;
    new_position = (*position + 1) % history->size;
    if (new_position == (history->end + 1) % history->size) {
        // Do not move past the end of history
        return;
    }

    *position = new_position;
    snprintf(line, MAX_TEXT_LENGTH, "%s", history->lines[*position]);
}

TextLineHistory* current_history(LocalPlayer *local)
{
    if (local->typing_buffer[0] == CRAFT_KEY_SIGN) {
        return &local->typing_history[SIGN_HISTORY];
    } else if (local->typing_buffer[0] == CRAFT_KEY_COMMAND) {
        return &local->typing_history[COMMAND_HISTORY];
    } else if (local->typing_buffer[0] == PW_KEY_LUA) {
        return &local->typing_history[LUA_HISTORY];
    } else {
        return &local->typing_history[CHAT_HISTORY];
    }
}

void get_history_path(int history_type, int player_id, char *path)
{
    switch (history_type) {
    case CHAT_HISTORY:
        snprintf(path, MAX_PATH_LENGTH, "%s/chat%d.history",
                 config->path, player_id);
        break;
    case COMMAND_HISTORY:
        snprintf(path, MAX_PATH_LENGTH, "%s/command%d.history",
                 config->path, player_id);
        break;
    case SIGN_HISTORY:
        snprintf(path, MAX_PATH_LENGTH, "%s/sign%d.history",
                 config->path, player_id);
        break;
    case LUA_HISTORY:
        snprintf(path, MAX_PATH_LENGTH, "%s/lua%d.history",
                 config->path, player_id);
        break;
    }
}

void history_load(void)
{
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = &g->local_players[i];
        for (int h=0; h<NUM_HISTORIES; h++) {
            char history_path[MAX_PATH_LENGTH];
            TextLineHistory *history = local->typing_history + h;
            FILE *fp;
            char buf[MAX_TEXT_LENGTH];
            get_history_path(h, local->player->id, history_path);
            fp = fopen(history_path, "r");
            if (fp == NULL) {
                continue;
            }
            while (fgets(buf, MAX_TEXT_LENGTH, fp) != NULL) {
                char *p = strchr(buf, '\n');
                if (p) *p = '\0';
                history_add(history, buf);
            }
            fclose(fp);
        }
    }
}

void history_save(void)
{
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = &g->local_players[i];
        for (int h=0; h<NUM_HISTORIES; h++) {
            char history_path[MAX_PATH_LENGTH];
            TextLineHistory *history = local->typing_history + h;
            get_history_path(h, local->player->id, history_path);
            if (history->size > 0) {
                FILE *fp;
                fp = fopen(history_path, "w");
                if (fp == NULL) {
                    return;
                }
                for (int j = 0; j < history->size; j++) {
                    fprintf(fp, "%s\n", history->lines[j]);
                }
                fclose(fp);
            }
        }
    }
}

void handle_key_press_typing(LocalPlayer *local, int mods, int keysym)
{
    switch (keysym) {
    case XK_Escape:
        local->typing = 0;
        local->history_position = NOT_IN_HISTORY;
        break;
    case XK_BackSpace:
        {
        size_t n = strlen(local->typing_buffer);
        if (n > 0 && local->text_cursor > local->typing_start) {
            if (local->text_cursor < n) {
                memmove(local->typing_buffer + local->text_cursor - 1,
                        local->typing_buffer + local->text_cursor,
                        n - local->text_cursor);
            }
            local->typing_buffer[n - 1] = '\0';
            local->text_cursor -= 1;
        }
        break;
        }
    case XK_Return:
        if (mods & ShiftMask) {
            insert_into_typing_buffer(local, '\r');
        } else {
            local->typing = 0;
            if (local->typing_buffer[0] == CRAFT_KEY_SIGN) {
                int x, y, z, face;
                if (hit_test_face(local->player, &x, &y, &z, &face)) {
                    set_sign(x, y, z, face, local->typing_buffer + 1);
                }
            } else if (local->typing_buffer[0] == CRAFT_KEY_COMMAND) {
                parse_command(local, local->typing_buffer, 1);
            } else if (local->typing_buffer[0] == PW_KEY_LUA) {
                pwlua_parse_line(local->lua_shell, local->typing_buffer);
            } else {
                client_talk(local->typing_buffer);
            }
            history_add(current_history(local), local->typing_buffer);
            local->history_position = NOT_IN_HISTORY;
        }
        break;
    case XK_Delete:
        {
        size_t n = strlen(local->typing_buffer);
        if (n > 0 && local->text_cursor < n) {
            memmove(local->typing_buffer + local->text_cursor,
                    local->typing_buffer + local->text_cursor + 1,
                    n - local->text_cursor);
            local->typing_buffer[n - 1] = '\0';
        }
        break;
        }
    case XK_Left:
        if (local->text_cursor > local->typing_start) {
            local->text_cursor -= 1;
        }
        break;
    case XK_Right:
        if (local->text_cursor < strlen(local->typing_buffer)) {
            local->text_cursor += 1;
        }
        break;
    case XK_Home:
        local->text_cursor = local->typing_start;
        break;
    case XK_End:
        local->text_cursor = strlen(local->typing_buffer);
        break;
    case XK_Up:
        history_previous(current_history(local), local->typing_buffer,
                         &local->history_position);
        local->text_cursor = strlen(local->typing_buffer);
        break;
    case XK_Down:
        history_next(current_history(local), local->typing_buffer,
                     &local->history_position);
        local->text_cursor = strlen(local->typing_buffer);
        break;
    default:
        if (keysym >= XK_space && keysym <= XK_asciitilde) {
            insert_into_typing_buffer(local, keysym);
        }
        break;
    }
}

void handle_key_press(int keyboard_id, int mods, int keysym)
{
    int prev_player_count = config->players;
    LocalPlayer *p = player_for_keyboard(keyboard_id);
    if (prev_player_count != config->players) {
        // If a new keyboard resulted in a new player ignore this first event.
        return;
    }
    if (p->typing) {
        handle_key_press_typing(p, mods, keysym);
        return;
    }
    if (p->active_menu) {
        int r = menu_handle_key_press(p->active_menu, mods, keysym);
        handle_menu_event(p, p->active_menu, r);
        return;
    }
    switch (keysym) {
    case XK_w: case XK_W:
        p->forward_is_pressed = 1;
        p->movement_speed_forward_back = 1;
        break;
    case XK_s: case XK_S:
        p->back_is_pressed = 1;
        p->movement_speed_forward_back = 1;
        break;
    case XK_a: case XK_A:
        p->left_is_pressed = 1;
        p->movement_speed_left_right = 1;
        break;
    case XK_d: case XK_D:
        p->right_is_pressed = 1;
        p->movement_speed_left_right = 1;
        break;
    case XK_Escape:
        if (p->active_menu) {
            close_menu(p);
        } else {
            open_menu(p, &p->menu);
        }
        break;
    case XK_i: case XK_I:
        open_menu(p, &p->menu_item_in_hand);
        break;
    case XK_F1:
        set_mouse_absolute();
        break;
    case XK_F2:
        set_mouse_relative();
        break;
    case XK_F8:
        {
        // Move local player keyboard and mouse to next active player
        int next = get_next_local_player(g->clients, p->player->id - 1);
        LocalPlayer *next_local = g->local_players + next;
        if (next_local != p) {
            next_local->keyboard_id = keyboard_id;
            p->keyboard_id = UNASSIGNED;
            if (p->mouse_id != UNASSIGNED) {
                next_local->mouse_id = p->mouse_id;
                p->mouse_id = UNASSIGNED;
            }
        }
        break;
        }
    case XK_F11:
        pg_toggle_fullscreen();
        break;
    case XK_Up:
        p->view_up_is_pressed = 1;
        p->view_speed_up_down = 1;
        break;
    case XK_Down:
        p->view_down_is_pressed = 1;
        p->view_speed_up_down = 1;
        break;
    case XK_Left:
        p->view_left_is_pressed = 1;
        p->view_speed_left_right = 1;
        break;
    case XK_Right:
        p->view_right_is_pressed = 1;
        p->view_speed_left_right = 1;
        break;
    case XK_space:
        p->jump_is_pressed = 1;
        break;
    case XK_c: case XK_C:
        p->crouch_is_pressed = 1;
        break;
    case XK_z: case XK_Z:
        p->zoom_is_pressed = 1;
        break;
    case XK_f: case XK_F:
        p->ortho_is_pressed = 1;
        break;
    case XK_Tab:
        if (!mods) {
            p->flying = !p->flying;
        }
        break;
    case XK_1: case XK_2: case XK_3: case XK_4: case XK_5: case XK_6:
    case XK_7: case XK_8: case XK_9:
        p->item_index = keysym - XK_1;
        break;
    case XK_0:
        p->item_index = 9;
        break;
    case XK_e: case XK_E:
        cycle_item_in_hand_up(p);
        break;
    case XK_r: case XK_R:
        cycle_item_in_hand_down(p);
        break;
    case XK_g: case XK_G:
        set_item_in_hand_to_item_under_crosshair(p);
        break;
    case XK_o: case XK_O:
        {
        int start = ((p->observe1 == 0) ?  p->player->id : p->observe1) - 1;
        if (p->observe1_client_id == 0) {
            p->observe1_client_id = g->clients->id;
        }
        get_next_player(&start, &p->observe1_client_id);
        p->observe1 = start + 1;
        if (p->observe1 == p->player->id &&
            p->observe1_client_id == g->clients->id) {
            // cancel observing of another player
            p->observe1 = 0;
            p->observe1_client_id = 0;
        }
        break;
        }
    case XK_p: case XK_P:
        {
        int start = ((p->observe2 == 0) ?  p->player->id : p->observe2) - 1;
        if (p->observe2_client_id == 0) {
            p->observe2_client_id = g->clients->id;
        }
        get_next_player(&start, &p->observe2_client_id);
        p->observe2 = start + 1;
        if (p->observe2 == p->player->id &&
            p->observe2_client_id == g->clients->id) {
            // cancel observing of another player
            p->observe2 = 0;
            p->observe2_client_id = 0;
        }
        break;
        }
    case XK_t: case XK_T:
        p->typing = 1;
        p->typing_buffer[0] = '\0';
        p->typing_start = p->typing_history[CHAT_HISTORY].line_start;
        p->text_cursor = p->typing_start;
        break;
    case XK_u: case XK_U:
        if (p->has_undo_block == 1) {
            UndoBlock *b = &p->undo_block;
            int bp = chunked(b->x);
            int bq = chunked(b->z);
            set_block(b->x, b->y, b->z, b->texture);
            set_extra(b->x, b->y, b->z, b->extra);
            set_light(bp, bq, b->x, b->y, b->z, b->light);
            set_shape(b->x, b->y, b->z, b->shape);
            set_transform(b->x, b->y, b->z, b->transform);
            if (b->has_sign) {
                SignList *signs = &b->signs;
                for (size_t i = 0; i < signs->size; i++) {
                    Sign *e = signs->data + i;
                    set_sign(e->x, e->y, e->z, e->face, e->text);
                }
                sign_list_free(signs);
                b->has_sign = 0;
            }
            p->has_undo_block = 0;
        }
        break;
    case XK_slash:
        p->typing = 1;
        p->typing_buffer[0] = CRAFT_KEY_COMMAND;
        p->typing_buffer[1] = '\0';
        p->typing_start = p->typing_history[COMMAND_HISTORY].line_start;
        p->text_cursor = p->typing_start;
        break;
    case XK_dollar:
        p->typing = 1;
        p->typing_buffer[0] = PW_KEY_LUA;
        p->typing_buffer[1] = '\0';
        p->typing_start = p->typing_history[LUA_HISTORY].line_start;
        p->text_cursor = p->typing_start;
        break;
    case XK_grave:
        p->typing = 1;
        p->typing_buffer[0] = CRAFT_KEY_SIGN;
        p->typing_buffer[1] = '\0';
        int x, y, z, face;
        if (hit_test_face(p->player, &x, &y, &z, &face)) {
            const unsigned char *existing_sign = get_sign(
                chunked(x), chunked(z), x, y, z, face);
            if (existing_sign) {
                strncpy(p->typing_buffer + 1, (char *)existing_sign,
                        MAX_TEXT_LENGTH - 1);
                p->typing_buffer[MAX_TEXT_LENGTH - 1] = '\0';
            }
        }
        p->typing_start = p->typing_history[SIGN_HISTORY].line_start;
        p->text_cursor = p->typing_start;
        break;
    case XK_Return:
        if (mods & ControlMask) {
            set_block_under_crosshair(p);
        } else {
            clear_block_under_crosshair(p);
        }
        break;
    }
}

void handle_key_release(int keyboard_id, int keysym)
{
    LocalPlayer *local = player_for_keyboard(keyboard_id);
    switch (keysym) {
    case XK_w: case XK_W:
        local->forward_is_pressed = 0;
        break;
    case XK_s: case XK_S:
        local->back_is_pressed = 0;
        break;
    case XK_a: case XK_A:
        local->left_is_pressed = 0;
        break;
    case XK_d: case XK_D:
        local->right_is_pressed = 0;
        break;
    case XK_space:
        local->jump_is_pressed = 0;
        break;
    case XK_c: case XK_C:
        local->crouch_is_pressed = 0;
        break;
    case XK_z: case XK_Z:
        local->zoom_is_pressed = 0;
        break;
    case XK_f: case XK_F:
        local->ortho_is_pressed = 0;
        break;
    case XK_Up:
        local->view_up_is_pressed = 0;
        break;
    case XK_Down:
        local->view_down_is_pressed = 0;
        break;
    case XK_Left:
        local->view_left_is_pressed = 0;
        break;
    case XK_Right:
        local->view_right_is_pressed = 0;
        break;
    }
}

LocalPlayer* player_for_keyboard(int keyboard_id) {
    // Match keyboard to assigned player
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = g->local_players + i;
        if (local->player->is_active && local->keyboard_id == keyboard_id) {
            return local;
        }
    }
    // Assign keyboard to next active player without one
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = g->local_players + i;
        if (local->player->is_active && local->keyboard_id == UNASSIGNED) {
            local->keyboard_id = keyboard_id;
            return local;
        }
    }
    // Add a new player and assign the keyboard to them
    if (g->auto_add_players_on_new_devices &&
        config->players < MAX_LOCAL_PLAYERS) {
        config->players++;
        if (limit_player_count_to_fit_gpu_mem() == 0) {
            LocalPlayer *local = &g->local_players[config->players - 1];
            local->player->is_active = 1;
            local->keyboard_id = keyboard_id;
            set_view_radius(g->render_radius, g->delete_radius);
            return local;
        }
    }
    // If keyboard remains unassigned and all active players already have one
    // assign this keyboard to the first player.
    return g->local_players;
}

LocalPlayer* player_for_mouse(int mouse_id) {
    // Match mouse to assigned player
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = g->local_players + i;
        if (local->player->is_active && local->mouse_id == mouse_id) {
            return local;
        }
    }
    // Assign mouse to next active player without a mouse and already assigned
    // a keyboard
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = g->local_players + i;
        if (local->player->is_active && local->mouse_id == UNASSIGNED &&
            local->keyboard_id != UNASSIGNED) {
            local->mouse_id = mouse_id;
            return local;
        }
    }
    // Assign mouse to next active player without one
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = g->local_players + i;
        if (local->player->is_active && local->mouse_id == UNASSIGNED) {
            local->mouse_id = mouse_id;
            return local;
        }
    }
    // If mouse remains unassigned and all active players already have one
    // assign this mouse to the first player.
    return g->local_players;
}

void open_menu(LocalPlayer *local, Menu *menu)
{
    local->active_menu = menu;
    menu_handle_mouse(local->active_menu, local->mouse_x, local->mouse_y);
    cancel_player_inputs(local);
}

void close_menu(LocalPlayer *local)
{
    local->active_menu = NULL;
}

void handle_menu_event(LocalPlayer *local, Menu *menu, int event)
{
    if (event == MENU_CANCELLED) {
        close_menu(local);
    } else if (menu == &local->menu) {
        if (event == local->menu_id_resume) {
            close_menu(local);
        } else if (event == local->menu_id_options) {
            open_menu(local, &local->menu_options);
        } else if (event == local->menu_id_new) {
            open_menu(local, &local->menu_new);
            menu_set_highlighted_item(&local->menu_new,
                local->menu_id_new_game_name);
        } else if (event == local->menu_id_load) {
            populate_load_menu(local);
            open_menu(local, &local->menu_load);
        } else if (event == local->menu_id_exit) {
            terminate = 1;
        }
    } else if (menu == &local->menu_options) {
        if (event == local->menu_id_options_resume) {
            close_menu(local);
        } else if (event == local->menu_id_script) {
            open_menu(local, &local->menu_script);
        } else if (event == local->menu_id_worldgen) {
            open_menu(local, &local->menu_worldgen);
        } else if (event == local->menu_id_fullscreen) {
            pg_toggle_fullscreen();
        } else if (event == local->menu_id_crosshairs) {
            config->show_crosshairs = menu_get_option(&local->menu_options,
                local->menu_id_crosshairs);
        } else if (event == local->menu_id_verbose) {
            config->verbose = menu_get_option(&local->menu_options,
                local->menu_id_verbose);
        } else if (event == local->menu_id_wireframe) {
            config->show_wireframe = menu_get_option(&local->menu_options,
                local->menu_id_wireframe);
        }
    } else if (menu == &local->menu_new) {
        if (event == local->menu_id_new_cancel) {
            close_menu(local);
        } else if (event == local->menu_id_new_ok ||
                   event == local->menu_id_new_game_name) {
            close_menu(local);
            char *new_game_name = menu_get_line_edit(menu,
                local->menu_id_new_game_name);
            char new_game_path[MAX_PATH_LENGTH];
            if (strlen(new_game_name) == 0) {
                add_message(local->player->id, "New game needs a name");
                return;
            }
            snprintf(new_game_path, MAX_PATH_LENGTH, "%s/%s.piworld",
                config->path, new_game_name);
            if (access(new_game_path, F_OK) != -1) {
                add_message(local->player->id,
                    "A game with that name already exists");
                return;
            }
            // Create a new game
            g->mode_changed = 1;
            g->mode = MODE_OFFLINE;
            snprintf(g->db_path, MAX_PATH_LENGTH, "%s", new_game_path);
        }
    } else if (menu == &local->menu_load) {
        if (event == local->menu_id_load_cancel) {
            close_menu(local);
        } else if (event > 0) {
            // Load an existing game
            char game_path[MAX_PATH_LENGTH];
            snprintf(game_path, MAX_PATH_LENGTH, "%s/%s.piworld", config->path,
                     menu_get_name(menu, event));
            if (access(game_path, F_OK) == -1) {
                add_message(local->player->id, "Game file not found");
                return;
            }
            g->mode_changed = 1;
            g->mode = MODE_OFFLINE;
            snprintf(g->db_path, MAX_PATH_LENGTH, "%s", game_path);
            close_menu(local);
        }
    } else if (menu == &local->menu_item_in_hand) {
        if (event == local->menu_id_item_in_hand_cancel) {
            close_menu(local);
        } else if (event > 0) {
            local->item_index = event - 1;
            close_menu(local);
        }
    } else if (menu == &local->menu_block_edit) {
        if (event == local->menu_id_block_edit_resume) {
            close_menu(local);
        } else if (event == local->menu_id_texture) {
            open_menu(local, &local->menu_texture);
        } else if (event == local->menu_id_shape) {
            open_menu(local, &local->menu_shape);
        } else if (event == local->menu_id_sign_text) {
            set_sign(local->edit_x, local->edit_y, local->edit_z,
                     local->edit_face, menu_get_line_edit(menu, event));
        } else if (event == local->menu_id_transform) {
            int w = atoi(menu_get_line_edit(menu, event));
            set_transform(local->edit_x, local->edit_y, local->edit_z, w);
        } else if (event == local->menu_id_light) {
            int w = atoi(menu_get_line_edit(menu, event));
            set_light(chunked(local->edit_x), chunked(local->edit_z),
                      local->edit_x, local->edit_y, local->edit_z, w);
        } else if (event == local->menu_id_control_bit) {
            int cb = menu_get_option(menu, local->menu_id_control_bit);
            int w = get_extra(local->edit_x, local->edit_y, local->edit_z);
            if (cb) {
                w |= EXTRA_BIT_CONTROL;
            } else {
                w &= ~EXTRA_BIT_CONTROL;
            }
            set_extra(local->edit_x, local->edit_y, local->edit_z, w);
        } else if (event == local->menu_id_open_bit) {
            int open = menu_get_option(menu, local->menu_id_open_bit);
            int w = get_extra(local->edit_x, local->edit_y, local->edit_z);
            if (open) {
                w |= EXTRA_BIT_OPEN;
            } else {
                w &= ~EXTRA_BIT_OPEN;
            }
            set_extra(local->edit_x, local->edit_y, local->edit_z, w);
        }
    } else if (menu == &local->menu_texture) {
        if (event == local->menu_id_texture_cancel) {
            close_menu(local);
        } else if (event > 0) {
            set_block(local->edit_x, local->edit_y, local->edit_z,
                      items[event - 1]);
            close_menu(local);
        }
    } else if (menu == &local->menu_shape) {
        if (event == local->menu_id_shape_cancel) {
            close_menu(local);
        } else if (event > 0) {
            set_shape(local->edit_x, local->edit_y, local->edit_z,
                      shapes[event - 1]);
            close_menu(local);
        }
    } else if (menu == &local->menu_script) {
        if (event == local->menu_id_script_cancel) {
            close_menu(local);
        } else if (event == local->menu_id_script_run) {
            populate_script_run_menu(local);
            open_menu(local, &local->menu_script_run);
        }
    } else if (menu == &local->menu_worldgen) {
        if (event == local->menu_id_worldgen_cancel) {
            close_menu(local);
        } else if (event == local->menu_id_worldgen_select) {
            populate_worldgen_select_menu(local);
            open_menu(local, &local->menu_worldgen_select);
        }
    } else if (menu == &local->menu_script_run) {
        if (event == local->menu_id_script_run_cancel) {
            close_menu(local);
        } else if (event > 0) {
            char path[MAX_PATH_LENGTH];
            struct stat sb;
            snprintf(path, MAX_PATH_LENGTH, "%s/%s",
                     local->menu_script_run_dir, menu_get_name(menu, event));
            char *tmp = realpath(path, NULL);
            snprintf(path, MAX_PATH_LENGTH, tmp);
            free(tmp);
            stat(path, &sb);
            if (S_ISDIR(sb.st_mode)) {
                snprintf(local->menu_script_run_dir, MAX_DIR_LENGTH, path);
                populate_script_run_menu(local);
                open_menu(local, &local->menu_script_run);
                return;
            }
            char lua_code[LUA_MAXINPUT];
            snprintf(lua_code, sizeof(lua_code), "$dofile(\"%s\")\n", path);
            LuaThreadState* new_lua_instance = pwlua_new(local->player->id);
            pwlua_parse_line(new_lua_instance, lua_code);
        }
    } else if (menu == &local->menu_worldgen_select) {
        if (event == local->menu_id_worldgen_select_cancel) {
            close_menu(local);
        } else if (event > 0) {
            set_worldgen(menu_get_name(menu, event));
            db_set_option("worldgen", menu_get_name(menu, event));
        }
    }
}

void handle_mouse_release(int mouse_id, int b)
{
    if (!relative_mouse_in_use()) {
        return;
    }
    LocalPlayer *local = player_for_mouse(mouse_id);
    int mods = 0;
    if (local->keyboard_id != UNASSIGNED) {
        mods = pg_get_mods(local->keyboard_id);
    }
    if (local->active_menu) {
        int r = menu_handle_mouse_release(local->active_menu, local->mouse_x,
                                            local->mouse_y, b);
        handle_menu_event(local, local->active_menu, r);
        return;
    }
    if (b == 1) {
        if (mods & ControlMask) {
            set_block_under_crosshair(local);
        } else {
            clear_block_under_crosshair(local);
        }
    } else if (b == 2) {
        open_menu_for_item_under_crosshair(local);
    } else if (b == 3) {
        if (mods & ControlMask) {
            on_light(local);
        } else {
            set_block_under_crosshair(local);
        }
    } else if (b == 4) {
        cycle_item_in_hand_up(local);
    } else if (b == 5) {
        cycle_item_in_hand_down(local);
    }
}

void handle_window_close(void)
{
    terminate = True;
}

void cancel_player_inputs(LocalPlayer *p)
{
    p->forward_is_pressed = 0;
    p->back_is_pressed = 0;
    p->left_is_pressed = 0;
    p->right_is_pressed = 0;
    p->jump_is_pressed = 0;
    p->crouch_is_pressed = 0;
    p->view_left_is_pressed = 0;
    p->view_right_is_pressed = 0;
    p->view_up_is_pressed = 0;
    p->view_down_is_pressed = 0;
    p->ortho_is_pressed = 0;
    p->zoom_is_pressed = 0;
}

void handle_focus_out(void) {
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *p = &g->local_players[i];
        cancel_player_inputs(p);
    }

    pg_fullscreen(0);
}

void reset_history(LocalPlayer *local)
{
    local->history_position = NOT_IN_HISTORY;
    for (int i=0; i<NUM_HISTORIES; i++) {
        local->typing_history[i].end = -1;
        local->typing_history[i].size = 0;
    }
    local->typing_history[CHAT_HISTORY].line_start = 0;
    local->typing_history[COMMAND_HISTORY].line_start = 1;
    local->typing_history[SIGN_HISTORY].line_start = 1;
    local->typing_history[LUA_HISTORY].line_start = 1;
}

LocalPlayer* player_for_joystick(int joystick_id) {
    // Match joystick to assigned player
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = g->local_players + i;
        if (local->player->is_active && local->joystick_id == joystick_id) {
            return local;
        }
    }
    // Assign joystick to next active player without one
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = g->local_players + i;
        if (local->player->is_active && local->joystick_id == UNASSIGNED) {
            local->joystick_id = joystick_id;
            return local;
        }
    }
    // Add a new player and assign the joystick to them
    if (g->auto_add_players_on_new_devices &&
        config->players < MAX_LOCAL_PLAYERS) {
        config->players++;
        if (limit_player_count_to_fit_gpu_mem() == 0) {
            LocalPlayer *local = &g->local_players[config->players - 1];
            local->player->is_active = 1;
            local->joystick_id = joystick_id;
            set_view_radius(g->render_radius, g->delete_radius);
            return local;
        }
    }
    // If joystick remains unassigned and all active players already have one
    // assign this joystick to the first player.
    return g->local_players;
}


void handle_joystick_axis(PG_Joystick *j, int j_num, int axis, float value)
{
    int prev_player_count = config->players;
    LocalPlayer *p = player_for_joystick(j_num);
    if (prev_player_count != config->players) {
        // If a new gamepad resulted in a new player ignore this first event.
        return;
    }
    if (p->active_menu) {
        menu_handle_joystick_axis(p->active_menu, axis, value);
        return;
    }

    if (j->axis_count < 4) {
        if (axis == 0) {
            p->left_is_pressed = (value < 0) ? 1 : 0;
            p->right_is_pressed = (value > 0) ? 1 : 0;
            p->movement_speed_left_right = fabs(value);
        } else if (axis == 1) {
            p->forward_is_pressed = (value < 0) ? 1 : 0;
            p->back_is_pressed = (value > 0) ? 1 : 0;
            p->movement_speed_forward_back = fabs(value);
        }
    } else {
        if (axis == 0) {
            p->movement_speed_left_right = fabs(value) * 2.0;
            p->left_is_pressed = (value < -DEADZONE);
            p->right_is_pressed = (value > DEADZONE);
        } else if (axis == 1) {
            p->movement_speed_forward_back = fabs(value) * 2.0;
            p->forward_is_pressed = (value < -DEADZONE);
            p->back_is_pressed = (value > DEADZONE);
        } else if (axis == 2) {
            p->view_speed_up_down = fabs(value) * 2.0;
            p->view_up_is_pressed = (value < -DEADZONE);
            p->view_down_is_pressed = (value > DEADZONE);
        } else if (axis == 3) {
            p->view_speed_left_right = fabs(value) * 2.0;
            p->view_left_is_pressed = (value < -DEADZONE);
            p->view_right_is_pressed = (value > DEADZONE);
        } else if (axis == 4) {
            p->movement_speed_left_right = fabs(value);
            p->left_is_pressed = (value < 0) ? 1 : 0;
            p->right_is_pressed = (value > 0) ? 1 : 0;
        } else if (axis == 5) {
            p->movement_speed_forward_back = fabs(value);
            p->forward_is_pressed = (value < 0) ? 1 : 0;
            p->back_is_pressed = (value > 0) ? 1 : 0;
        }
    }
}

void handle_joystick_button(PG_Joystick *j, int j_num, int button, int state)
{
    int prev_player_count = config->players;
    LocalPlayer *p = player_for_joystick(j_num);
    if (prev_player_count != config->players) {
        // If a new gamepad resulted in a new player ignore this first event.
        return;
    }
    if (p->active_menu) {
        int r = menu_handle_joystick_button(p->active_menu, button, state);
        handle_menu_event(p, p->active_menu, r);
        return;
    }

    if (j->axis_count < 4) {
        if (button == 0) {
            p->view_up_is_pressed = state;
            p->view_speed_up_down = 1;
        } else if (button == 2) {
            p->view_down_is_pressed = state;
            p->view_speed_up_down = 1;
        } else if (button == 3) {
            p->view_left_is_pressed = state;
            p->view_speed_left_right = 1;
        } else if (button == 1) {
            p->view_right_is_pressed = state;
            p->view_speed_left_right = 1;
        } else if (button == 5) {
            switch (p->shoulder_button_mode) {
                case CROUCH_JUMP:
                    p->jump_is_pressed = state;
                    break;
                case REMOVE_ADD:
                    if (state) {
                        set_block_under_crosshair(p);
                    }
                    break;
                case CYCLE_DOWN_UP:
                    if (state) {
                        cycle_item_in_hand_up(p);
                    }
                    break;
                case FLY_PICK:
                    if (state) {
                        set_item_in_hand_to_item_under_crosshair(p);
                    }
                    break;
                case ZOOM_ORTHO:
                    p->ortho_is_pressed = state;
                    break;
            }
        } else if (button == 4) {
           switch (p->shoulder_button_mode) {
                case CROUCH_JUMP:
                    p->crouch_is_pressed = state;
                    break;
                case REMOVE_ADD:
                    if (state) {
                        clear_block_under_crosshair(p);
                    }
                    break;
                case CYCLE_DOWN_UP:
                    if (state) {
                        cycle_item_in_hand_down(p);
                    }
                    break;
                case FLY_PICK:
                    if (state) {
                        p->flying = !p->flying;
                    }
                    break;
                case ZOOM_ORTHO:
                    p->zoom_is_pressed = state;
                    break;
            }
        } else if (button == 8) {
            if (state) {
                p->shoulder_button_mode += 1;
                p->shoulder_button_mode %= SHOULDER_BUTTON_MODE_COUNT;
                add_message(p->player->id,
                            shoulder_button_modes[p->shoulder_button_mode]);
            }
        } else if (button == 9) {
            if (state) {
                if (p->active_menu) {
                    close_menu(p);
                } else {
                    open_menu(p, &p->menu);
                }
            }
        }
    } else {
        if (button == 0) {
            if (state) {
                cycle_item_in_hand_up(p);
            }
        } else if (button == 2) {
            if (state) {
                cycle_item_in_hand_down(p);
            }
        } else if (button == 3) {
            if (state) {
                set_item_in_hand_to_item_under_crosshair(p);
            }
        } else if (button == 1) {
            if (state) {
                p->flying = !p->flying;
            }
        } else if (button == 5) {
            if (state) {
                clear_block_under_crosshair(p);
            }
        } else if (button == 4) {
            p->crouch_is_pressed = state;
        } else if (button == 6) {
            p->jump_is_pressed = state;
        } else if (button == 7) {
            if (state) {
                set_block_under_crosshair(p);
            }
        } else if (button == 8) {
            p->zoom_is_pressed = state;
        } else if (button == 9) {
            if (state) {
                if (p->active_menu) {
                    close_menu(p);
                } else {
                    open_menu(p, &p->menu);
                }
            }
        } else if (button == 10) {
            p->ortho_is_pressed = state;
        } else if (button == 11) {
            p->zoom_is_pressed = state;
        }
    }
}

void render_player_world(
        LocalPlayer *local, GLuint sky_buffer, Attrib *sky_attrib,
        Attrib *block_attrib, Attrib *text_attrib, Attrib *line_attrib,
        Attrib *mouse_attrib, FPS fps)
{
    Player *player = local->player;
    State *s = &player->state;

    glViewport(local->view_x, local->view_y, local->view_width,
               local->view_height);
    g->width = local->view_width;
    g->height = local->view_height;
    g->ortho = local->ortho_is_pressed ? 64 : 0;
    g->fov = local->zoom_is_pressed ? 15 : 65;

    if (local->observe1 > 0 && find_client(local->observe1_client_id)) {
        player = find_client(local->observe1_client_id)->players +
                 (local->observe1 - 1);
    }

    // RENDER 3-D SCENE //
    render_sky(sky_attrib, player, sky_buffer);
    glClear(GL_DEPTH_BUFFER_BIT);
    int face_count = render_chunks(block_attrib, player);
    render_signs(text_attrib, player);
    render_sign(text_attrib, local);
    render_players(block_attrib, player);
    if (config->show_wireframe) {
        render_wireframe(line_attrib, player);
    }

    // RENDER HUD //
    glClear(GL_DEPTH_BUFFER_BIT);
    if (config->show_crosshairs && local->active_menu == NULL) {
        render_crosshairs(line_attrib, player);
    }
    if (config->show_item) {
        render_item(block_attrib, local);
    }

    // RENDER TEXT //
    char text_buffer[1024];
    float ts = 8 * g->scale;
    float tx = ts / 2;
    float ty = g->height - ts;
    if (config->show_info_text) {
        int hour = time_of_day() * 24;
        char am_pm = hour < 12 ? 'a' : 'p';
        hour = hour % 12;
        hour = hour ? hour : 12;
        if (config->verbose) {
            snprintf(
                text_buffer, 1024,
                "(%d, %d) (%.2f, %.2f, %.2f) [%d, %d, %d] %d%cm",
                chunked(s->x), chunked(s->z), s->x, s->y, s->z,
                g->client_count, g->chunk_count,
                face_count * 2, hour, am_pm);
            render_text(text_attrib, ALIGN_LEFT, tx, ty, ts,
                        text_buffer);
            ty -= ts * 2;

            // FPS counter in lower right corner
            float bottom_bar_y = 0 + ts * 2;
            float right_side = g->width - ts;
            snprintf(text_buffer, 1024, "%dfps", fps.fps);
            render_text(text_attrib, ALIGN_RIGHT, right_side,
                        bottom_bar_y, ts, text_buffer);
        } else {
            snprintf(text_buffer, 1024, "(%.2f, %.2f, %.2f)",
                     s->x, s->y, s->z);
            render_text(text_attrib, ALIGN_LEFT, tx, ty, ts,
                        text_buffer);

            // Game time in upper right corner
            float right_side = g->width - tx;
            snprintf(text_buffer, 1024, "%d%cm", hour, am_pm);
            render_text(text_attrib, ALIGN_RIGHT, right_side, ty, ts,
                        text_buffer);

            ty -= ts * 2;
        }
    }
    if (config->show_chat_text) {
        for (int i = 0; i < MAX_MESSAGES; i++) {
            int index = (local->message_index + i) % MAX_MESSAGES;
            if (strlen(local->messages[index])) {
                render_text(text_attrib, ALIGN_LEFT, tx, ty, ts,
                    local->messages[index]);
                ty -= ts * 2;
            }
        }
    }
    if (local->typing) {
        snprintf(text_buffer, 1024, "> %s", local->typing_buffer);
        render_text(text_attrib, ALIGN_LEFT, tx, ty, ts, text_buffer);
        glClear(GL_DEPTH_BUFFER_BIT);
        render_text_cursor(line_attrib,
                           tx + ts * (local->text_cursor+1) + ts/2, ty);
        ty -= ts * 2;
    }
    if (config->show_player_names) {
        Player *other = player_crosshair(player);
        if (other) {
            render_text(text_attrib, ALIGN_CENTER,
                g->width / 2, g->height / 2 - ts - 24, ts,
                other->name);
        }
    }
    if (config->players > 1) {
        // Render player name if more than 1 local player
        snprintf(text_buffer, 1024, "%s", player->name);
        render_text(text_attrib, ALIGN_CENTER, g->width/2, ts, ts,
                    text_buffer);
    }
    if (local->active_menu) {
        glClear(GL_DEPTH_BUFFER_BIT);
        menu_render(local->active_menu, text_attrib, line_attrib,
                    g->width, g->height);
        glClear(GL_DEPTH_BUFFER_BIT);
        render_mouse_cursor(mouse_attrib, local->mouse_x, local->mouse_y,
                            local->player->id);
    }

    // RENDER PICTURE IN PICTURE //
    if (local->observe2 && find_client(local->observe2_client_id)) {
        player = (find_client(local->observe2_client_id))->players +
                 (local->observe2 - 1);

        int pw = local->view_width / 4 * g->scale;
        int ph = local->view_height / 3 * g->scale;
        int offset = 32 * g->scale;
        int pad = 3 * g->scale;
        int sw = pw + pad * 2;
        int sh = ph + pad * 2;

        glEnable(GL_SCISSOR_TEST);
        glScissor(g->width - sw - offset + pad + local->view_x,
                  offset - pad + local->view_y, sw, sh);
        glClear(GL_COLOR_BUFFER_BIT);
        glDisable(GL_SCISSOR_TEST);
        glClear(GL_DEPTH_BUFFER_BIT);
        glViewport(g->width - pw - offset + local->view_x,
                   offset + local->view_y, pw, ph);

        g->width = pw;
        g->height = ph;
        g->ortho = 0;
        g->fov = 65;

        render_sky(sky_attrib, player, sky_buffer);
        glClear(GL_DEPTH_BUFFER_BIT);
        render_chunks(block_attrib, player);
        render_signs(text_attrib, player);
        render_players(block_attrib, player);
        glClear(GL_DEPTH_BUFFER_BIT);
        if (config->show_player_names) {
            render_text(text_attrib, ALIGN_CENTER,
                pw / 2, ts, ts, player->name);
        }
    }
}

void set_players_view_size(int w, int h)
{
    int view_margin = 6;
    int active_count = 0;
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = &g->local_players[i];
        if (local->player->is_active) {
            active_count++;
            if (config->players == 1) {
                // Full size view
                local->view_x = 0;
                local->view_y = 0;
                local->view_width = w;
                local->view_height = h;
            } else if (config->players == 2) {
                // Half size views
                if (active_count == 1) {
                    local->view_x = 0;
                    local->view_y = 0;
                    local->view_width = w / 2 - (view_margin/2);
                    local->view_height = h;
                } else {
                    local->view_x = w / 2 + (view_margin/2);
                    local->view_y = 0;
                    local->view_width = w / 2 - (view_margin/2);
                    local->view_height = h;
                }
            } else {
                // Quarter size views
                if (local->player->id == 1) {
                    local->view_x = 0;
                    local->view_y = h / 2 + (view_margin/2);
                    local->view_width = w / 2 - (view_margin/2);
                    local->view_height = h / 2 - (view_margin/2);
                } else if (local->player->id == 2) {
                    local->view_x = w / 2 + (view_margin/2);
                    local->view_y = h / 2 + (view_margin/2);
                    local->view_width = w / 2 - (view_margin/2);
                    local->view_height = h / 2 - (view_margin/2);

                } else if (local->player->id == 3) {
                    local->view_x = 0;
                    local->view_y = 0;
                    local->view_width = w / 2 - (view_margin/2);
                    local->view_height = h / 2 - (view_margin/2);

                } else {
                    local->view_x = w / 2 + (view_margin/2);
                    local->view_y = 0;
                    local->view_width = w / 2 - (view_margin/2);
                    local->view_height = h / 2 - (view_margin/2);
                }
            }
        }
    }
}

int keyboard_player_count(void)
{
    int count = 0;
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = g->local_players + i;
        if (local->player && local->player->is_active &&
            local->keyboard_id != UNASSIGNED) {
            count++;
        }
    }
    return count;
}

void set_players_to_match_joysticks(void)
{
    int joystick_count = pg_joystick_count();
    if (joystick_count != config->players) {
        config->players = MAX(keyboard_player_count(), pg_joystick_count());
        config->players = MAX(1, config->players);
        config->players = MIN(MAX_LOCAL_PLAYERS, config->players);
        limit_player_count_to_fit_gpu_mem();
        set_player_count(g->clients, config->players);
    }
}

/*
 * Set view radius that will fit into the current size of GPU RAM.
 */
void set_view_radius(int requested_size, int delete_request)
{
    int radius = requested_size;
    int delete_radius = delete_request;
    if (!config->no_limiters) {
        int gpu_mb = pg_get_gpu_mem_size();
        if (gpu_mb < 48 || (gpu_mb < 64 && config->players >= 2) ||
            (gpu_mb < 128 && config->players >= 4)) {
            // A draw distance of 1 is not enough for the game to be usable,
            // but this does at least show something on screen (for low
            // resolutions only - higher ones will crash the game with low GPU
            // RAM).
            radius = 1;
            delete_radius = radius + 1;
        } else if (gpu_mb < 64 || (gpu_mb < 128 && config->players >= 3)) {
            radius = 2;
            delete_radius = radius + 1;
        } else if (gpu_mb < 128 && config->players >= 2) {
            radius = 2;
            delete_radius = radius + 2;
        } else if (gpu_mb < 128 || (gpu_mb < 256 && config->players >= 3)) {
            // A GPU RAM size of 64M will result in rendering issues for draw
            // distances greater than 3 (with a chunk size of 16).
            radius = 3;
            delete_radius = radius + 2;
        } else if (gpu_mb < 256 || requested_size == AUTO_PICK_RADIUS) {
            // For the Raspberry Pi reduce amount to draw to both fit into
            // 128MiB of GPU RAM and keep the render speed at a reasonable
            // smoothness.
            radius = 5;
            delete_radius = radius + 3;
        }
    }

    if (radius <= 0) {
        radius = 5;
    }

    if (delete_radius < radius) {
        delete_radius = radius + 3;
    }

    g->create_radius = radius;
    g->render_radius = radius;
    g->delete_radius = delete_radius;
    g->sign_radius = radius;

    if (config->verbose) {
        printf("\nradii: create: %d render: %d delete: %d sign: %d\n",
               g->create_radius, g->render_radius, g->delete_radius,
               g->sign_radius);
    }
}

void pw_get_player_pos(int player_id, float *x, float *y, float *z)
{
    if (player_id < 1 || player_id > MAX_LOCAL_PLAYERS) {
        return;
    }
    State *s = &g->local_players[player_id - 1].player->state;
    *x = s->x;
    *y = s->y;
    *z = s->z;
}

void pw_set_player_pos(int player_id, float x, float y, float z)
{
    if (player_id < 1 || player_id > MAX_LOCAL_PLAYERS) {
        return;
    }
    State *s = &g->local_players[player_id - 1].player->state;
    s->x = x;
    s->y = y;
    s->z = z;
}

void pw_get_player_angle(int player_id, float *x, float *y)
{
    if (player_id < 1 || player_id > MAX_LOCAL_PLAYERS) {
        return;
    }
    State *s = &g->local_players[player_id - 1].player->state;
    *x = s->rx;
    *y = s->ry;
}

void pw_set_player_angle(int player_id, float x, float y)
{
    if (player_id < 1 || player_id > MAX_LOCAL_PLAYERS) {
        return;
    }
    State *s = &g->local_players[player_id - 1].player->state;
    s->rx = x;
    s->ry = y;
}

int pw_get_time(void)
{
    return time_of_day() * 24;
}

void pw_set_time(int time)
{
    pg_set_time(g->day_length / (24.0 / (time == 0 ? 24 : time)));
    g->time_changed = 1;
}

void drain_edit_queue(size_t max_items, double max_time, double now)
{
    if (!ring_empty(&g->edit_ring) && mtx_trylock(&edit_ring_mtx)) {
        RingEntry e;
        for (size_t i=0; i<max_items; i++) {
            int edit_ready = ring_get(&g->edit_ring, &e);
            if (edit_ready) {
                switch (e.type) {
                case BLOCK:
                    set_block(e.x, e.y, e.z, e.w);
                    break;
                case EXTRA:
                    set_extra(e.x, e.y, e.z, e.w);
                    break;
                case SHAPE:
                    set_shape(e.x, e.y, e.z, e.w);
                    break;
                case LIGHT:
                    set_light(chunked(e.x), chunked(e.z), e.x, e.y, e.z, e.w);
                    break;
                case SIGN:
                    set_sign(e.x, e.y, e.z, e.w, e.sign);
                    free(e.sign);
                    break;
                case TRANSFORM:
                    set_transform(e.x, e.y, e.z, e.w);
                    break;
                case KEY:
                case COMMIT:
                case EXIT:
                default:
                    printf("Edit ring does not support: %d\n", e.type);
                    break;
                }
            } else {
                break;
            }
            if (pg_get_time() - now > max_time) {
                break;
            }
        }
        mtx_unlock(&edit_ring_mtx);
    }
}

void move_players_to_empty_block(void)
{
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        g->local_players[i].player = &g->clients->players[i];
        LocalPlayer *local = &g->local_players[i];
        State *s = &local->player->state;
        if (local->player->is_active) {
            int w = get_block(roundf(s->x), s->y, roundf(s->z));
            int shape = get_shape(roundf(s->x), s->y, roundf(s->z));
            int extra = get_extra(roundf(s->x), s->y, roundf(s->z));
            if (is_obstacle(w, shape, extra)) {
                s->y = highest_block(s->x, s->z) + 2;
            }
        }
    }
}

void initialize_worker_threads(void)
{
    for (int i = 0; i < WORKERS; i++) {
        Worker *worker = g->workers + i;
        worker->index = i;
        worker->state = WORKER_IDLE;
        worker->exit_requested = False;
        mtx_init(&worker->mtx, mtx_plain);
        cnd_init(&worker->cnd);
        thrd_create(&worker->thrd, worker_run, worker);
    }
}

void deinitialize_worker_threads(void)
{
    // Stop thread processing
    for (int i = 0; i < WORKERS; i++) {
        Worker *worker = g->workers + i;
        worker->exit_requested = True;
        cnd_signal(&worker->cnd);
    }
    // Wait for worker threads to exit
    for (int i = 0; i < WORKERS; i++) {
        Worker *worker = g->workers + i;
        thrd_join(worker->thrd, NULL);
        cnd_destroy(&worker->cnd);
        mtx_destroy(&worker->mtx);
    }
}

int main(int argc, char **argv) {
    int override_worldgen_from_command_line = 0;
    // INITIALIZATION //
    init_data_dir();
    srand(time(NULL));
    rand();
    reset_config();
    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = &g->local_players[i];
        reset_history(local);
    }
    parse_startup_config(argc, argv);
    if (strlen(config->worldgen_path) > 0) {
        set_worldgen(config->worldgen_path);
        override_worldgen_from_command_line = 1;
    }

    if (config->benchmark_create_chunks) {
        if (config->benchmark_create_chunks < MAX_CHUNKS &&
            config->benchmark_create_chunks > 0) {
            for (int i=0; i<config->benchmark_create_chunks; i++) {
                Chunk *chunk = g->chunks + i;
                create_chunk(chunk, i, i);
            }
        } else {
            printf("Invalid chunk count: %d\n",
                   config->benchmark_create_chunks);
        }
        return EXIT_SUCCESS;
    }

    if (config->lua_standalone) {
        pwlua_standalone_REPL();
        return EXIT_SUCCESS;  //TODO: exit status of lua instance
    }

    if (config->use_hfloat) {
        g->gl_float_type = GL_HALF_FLOAT_OES;
        g->float_size = sizeof(hfloat);
    } else {
        g->gl_float_type = GL_FLOAT;
        g->float_size = sizeof(GLfloat);
    }

    // SHAPE INITIALIZATION //
    fence_init();

    // WINDOW INITIALIZATION //
    g->width = config->window_width;
    g->height = config->window_height;
    pg_start(config->window_title, config->window_x, config->window_y,
             g->width, g->height);
    pg_swap_interval(config->vsync);
    set_key_press_handler(*handle_key_press);
    set_key_release_handler(*handle_key_release);
    set_mouse_release_handler(*handle_mouse_release);
    set_mouse_motion_handler(*handle_mouse_motion);
    set_window_close_handler(*handle_window_close);
    set_focus_out_handler(*handle_focus_out);
    if (config->verbose) {
        pg_print_info();
    }
    pg_init_joysticks();
    if (config->players == -1) {
        g->auto_add_players_on_new_devices = 1;
        set_players_to_match_joysticks();
    } else {
        g->auto_add_players_on_new_devices = 0;
        limit_player_count_to_fit_gpu_mem();
        set_player_count(g->clients, config->players);
    }
    pg_set_fullscreen_size(config->fullscreen_width, config->fullscreen_height);
    if (config->fullscreen) {
        pg_fullscreen(1);
    }
    set_view_radius(config->view, config->delete_radius);

    pg_set_joystick_button_handler(*handle_joystick_button);
    pg_set_joystick_axis_handler(*handle_joystick_axis);

    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = &g->local_players[i];
        create_menus(local);
    }

    glEnable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 1);

    // LOAD TEXTURES //
    GLuint texture;
    glGenTextures(1, &texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    load_texture("texture");

    GLuint sky;
    glGenTextures(1, &sky);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, sky);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    load_texture("sky");

    GLuint sign;
    glGenTextures(1, &sign);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, sign);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    load_texture("sign");

    // LOAD SHADERS //
    Attrib block_attrib = {0};
    Attrib line_attrib = {0};
    Attrib text_attrib = {0};
    Attrib sky_attrib = {0};
    Attrib mouse_attrib = {0};
    GLuint program;

    program = load_program("block");
    block_attrib.program = program;
    block_attrib.position = glGetAttribLocation(program, "position");
    block_attrib.normal = glGetAttribLocation(program, "normal");
    block_attrib.uv = glGetAttribLocation(program, "uv");
    block_attrib.matrix = glGetUniformLocation(program, "matrix");
    block_attrib.sampler = glGetUniformLocation(program, "sampler");
    block_attrib.extra1 = glGetUniformLocation(program, "sky_sampler");
    block_attrib.extra2 = glGetUniformLocation(program, "daylight");
    block_attrib.extra3 = glGetUniformLocation(program, "fog_distance");
    block_attrib.extra4 = glGetUniformLocation(program, "ortho");
    block_attrib.camera = glGetUniformLocation(program, "camera");
    block_attrib.timer = glGetUniformLocation(program, "timer");
    block_attrib.map = glGetUniformLocation(program, "map");

    program = load_program("line");
    line_attrib.program = program;
    line_attrib.position = glGetAttribLocation(program, "position");
    line_attrib.matrix = glGetUniformLocation(program, "matrix");
    line_attrib.extra1 = glGetUniformLocation(program, "color");

    program = load_program("text");
    text_attrib.program = program;
    text_attrib.position = glGetAttribLocation(program, "position");
    text_attrib.uv = glGetAttribLocation(program, "uv");
    text_attrib.color = glGetAttribLocation(program, "color");
    text_attrib.matrix = glGetUniformLocation(program, "matrix");
    text_attrib.sampler = glGetUniformLocation(program, "sampler");
    text_attrib.extra1 = glGetUniformLocation(program, "is_sign");
    text_attrib.extra2 = glGetUniformLocation(program, "sky_sampler");
    text_attrib.extra3 = glGetUniformLocation(program, "fog_distance");
    text_attrib.extra4 = glGetUniformLocation(program, "ortho");
    text_attrib.extra5 = glGetUniformLocation(program, "hud_text_background");
    text_attrib.extra6 = glGetUniformLocation(program, "hud_text_color");
    text_attrib.timer = glGetUniformLocation(program, "timer");
    text_attrib.camera = glGetUniformLocation(program, "camera");

    program = load_program("sky");
    sky_attrib.program = program;
    sky_attrib.position = glGetAttribLocation(program, "position");
    sky_attrib.uv = glGetAttribLocation(program, "uv");
    sky_attrib.matrix = glGetUniformLocation(program, "matrix");
    sky_attrib.sampler = glGetUniformLocation(program, "sampler");
    sky_attrib.timer = glGetUniformLocation(program, "timer");

    program = load_program("mouse");
    mouse_attrib.program = program;
    mouse_attrib.position = glGetAttribLocation(program, "position");
    mouse_attrib.uv = glGetAttribLocation(program, "uv");
    mouse_attrib.matrix = glGetUniformLocation(program, "matrix");
    mouse_attrib.sampler = glGetUniformLocation(program, "sampler");

    // ONLINE STATUS //
    if (strlen(config->server) > 0) {
        g->mode = MODE_ONLINE;
        get_server_db_cache_path(g->db_path);
    } else {
        g->mode = MODE_OFFLINE;
        snprintf(g->db_path, MAX_PATH_LENGTH, "%s", config->db_path);
    }

    mtx_init(&edit_ring_mtx, mtx_plain);

    // OUTER LOOP //
    int running = 1;
    while (running) {

        // INITIALIZE WORKER THREADS
        initialize_worker_threads();

        // DATABASE INITIALIZATION //
        if (g->mode == MODE_OFFLINE || config->use_cache) {
            db_enable();
            if (db_init(g->db_path)) {
                return EXIT_FAILURE;
            }
            if (g->mode == MODE_ONLINE) {
                // TODO: support proper caching of signs (handle deletions)
                db_delete_all_signs();
            } else {
                // Setup worldgen from local config
                const unsigned char *value;
                if (!override_worldgen_from_command_line) {
                    value = db_get_option("worldgen");
                    if (value != NULL) {
                        snprintf(config->worldgen_path, MAX_PATH_LENGTH, "%s", value);
                        set_worldgen(config->worldgen_path);
                    } else {
                        set_worldgen(NULL);
                    }
                } else if (strlen(config->worldgen_path) == 0) {
                    set_worldgen(NULL);
                }
                override_worldgen_from_command_line = 0;
                value = db_get_option("show-clouds");
                if (value != NULL) {
                    config->show_clouds = atoi((char *)value);
                }
                value = db_get_option("show-plants");
                if (value != NULL) {
                    config->show_plants = atoi((char *)value);
                }
                value = db_get_option("show-trees");
                if (value != NULL) {
                    config->show_trees = atoi((char *)value);
                }
            }
        }

        // CLIENT INITIALIZATION //
        if (g->mode == MODE_ONLINE) {
            client_enable();
            client_connect(config->server, config->port);
            client_start();
            client_version(2);
            login();
        }

        // LOCAL VARIABLES //
        reset_model();
        FPS fps = {0, 0, 0};
        double last_commit = pg_get_time();
        double last_update = pg_get_time();
        GLuint sky_buffer = gen_sky_buffer();

        g->client_count = 1;
        g->clients->id = 0;
        int check_players_position = 1;

        for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
            g->local_players[i].player = &g->clients->players[i];
            LocalPlayer *local = &g->local_players[i];
            State *s = &local->player->state;

            local->player->id = i+1;
            local->player->name[0] = '\0';
            local->player->buffer = 0;
            local->player->texture_index = i;

            local->mouse_id = UNASSIGNED;
            local->keyboard_id = UNASSIGNED;
            local->joystick_id = UNASSIGNED;

            // LOAD STATE FROM DATABASE //
            int loaded = db_load_state(&s->x, &s->y, &s->z, &s->rx, &s->ry, i);
            force_chunks(local->player);

            loaded = db_load_player_name(local->player->name, MAX_NAME_LENGTH, i);
            if (!loaded) {
                snprintf(local->player->name, MAX_NAME_LENGTH, "player%d", i + 1);
            }

            if (local->lua_shell == NULL) {
                local->lua_shell = pwlua_new_shell(local->player->id);
            }
        }

        for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
            Player *player = g->local_players[i].player;
            if (player->is_active) {
                client_add_player(player->id);
            }
        }

        history_load();

        // VIEW SETUP //
        g->ortho = 0;
        g->fov = 65;

        // BEGIN MAIN LOOP //
        double previous = pg_get_time();
        while (!terminate) {
            // WINDOW SIZE AND SCALE //
            g->scale = get_scale_factor();
            pg_get_window_size(&g->width, &g->height);
            glViewport(0, 0, g->width, g->height);
            set_players_view_size(g->width, g->height);

            // FRAME RATE //
            if (g->time_changed) {
                g->time_changed = 0;
                last_commit = pg_get_time();
                last_update = pg_get_time();
                memset(&fps, 0, sizeof(fps));
            }
            update_fps(&fps);
            double now = pg_get_time();
            double dt = now - previous;
            dt = MIN(dt, 0.2);
            dt = MAX(dt, 0.0);
            previous = now;

            // DRAIN EDIT QUEUE //
            drain_edit_queue(100000, 0.005, now);

            pwlua_remove_closed_threads();

            // HANDLE MOVEMENT //
            for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
                LocalPlayer *local = &g->local_players[i];
                if (local->player->is_active) {
                    handle_movement(dt, local);
                }
            }

            // HANDLE JOYSTICK INPUT //
            pg_poll_joystick_events();

            // HANDLE DATA FROM SERVER //
            char *buffer = client_recv();
            if (buffer) {
                parse_buffer(buffer);
                free(buffer);
            }

            // FLUSH DATABASE //
            if (now - last_commit > COMMIT_INTERVAL) {
                last_commit = now;
                db_commit();
            }

            // SEND POSITION TO SERVER //
            if (now - last_update > 0.1) {
                last_update = now;
                for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
                    LocalPlayer *local = &g->local_players[i];
                    if (local->player->is_active) {
                        State *s = &local->player->state;
                        client_position(local->player->id,
                                        s->x, s->y, s->z, s->rx, s->ry);
                    }
                }
            }

            // PREPARE TO RENDER //
            delete_chunks();
            for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
                Player *player = g->local_players[i].player;
                if (player->is_active) {
                    State *s = &player->state;
                    del_buffer(player->buffer);
                    player->buffer = gen_player_buffer(s->x, s->y, s->z, s->rx,
                        s->ry, player->texture_index);
                }
            }
            for (int i = 1; i < g->client_count; i++) {
                Client *client = g->clients + i;
                for (int j = 0; j < MAX_LOCAL_PLAYERS; j++) {
                    Player *remote_player = client->players + j;
                    if (remote_player->is_active) {
                        interpolate_player(remote_player);
                    }
                }
            }

            // RENDER //
            glClear(GL_COLOR_BUFFER_BIT);
            glClear(GL_DEPTH_BUFFER_BIT);
            for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
                LocalPlayer *local = &g->local_players[i];
                if (local->player->is_active) {
                    render_player_world(local, sky_buffer,
                                        &sky_attrib, &block_attrib, &text_attrib,
                                        &line_attrib, &mouse_attrib, fps);
                }
            }

#ifdef DEBUG
            check_gl_error();
#endif

            // SWAP AND POLL //
            pg_swap_buffers();
            pg_next_event();
            if (g->mode_changed) {
                g->mode_changed = 0;
                break;
            }
            if (check_players_position) {
                move_players_to_empty_block();
                check_players_position = 0;
            }
            if (g->render_option_changed) {
                check_players_position = 1;
                g->render_option_changed = 0;
                deinitialize_worker_threads();
                initialize_worker_threads();
                delete_all_chunks();
            }
        }
        if (terminate) {
            running = 0;
        }

        // DEINITIALIZE WORKER THREADS
        deinitialize_worker_threads();

        // SHUTDOWN //
        history_save();
        db_clear_state();
        db_clear_player_names();
        for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
            State *ls = &g->local_players[i].player->state;
            db_save_state(ls->x, ls->y, ls->z, ls->rx, ls->ry);
            db_save_player_name(g->local_players[i].player->name);
        }
        char time_str[16];
        snprintf(time_str, 16, "%f", time_of_day());
        db_set_option("time", time_str);
        db_close();
        db_disable();
        client_stop();
        client_disable();
        del_buffer(sky_buffer);
        delete_all_chunks();
        delete_all_players();
        ring_free(&g->edit_ring);
        set_worldgen(NULL);
    }
    mtx_destroy(&edit_ring_mtx);

    for (int i=0; i<MAX_LOCAL_PLAYERS; i++) {
        LocalPlayer *local = &g->local_players[i];
        menu_clear_items(&local->menu);
        menu_clear_items(&local->menu_options);
        menu_clear_items(&local->menu_new);
        menu_clear_items(&local->menu_load);
        menu_clear_items(&local->menu_item_in_hand);
        menu_clear_items(&local->menu_block_edit);
        menu_clear_items(&local->menu_texture);
        menu_clear_items(&local->menu_shape);
        menu_clear_items(&local->menu_script);
        menu_clear_items(&local->menu_script_run);
        menu_clear_items(&local->menu_worldgen);
        menu_clear_items(&local->menu_worldgen_select);
        pwlua_remove(local->lua_shell);
    }

    if (g->use_lua_worldgen == 1) {
        lua_close(g->lua_worldgen);
    }
    pg_terminate_joysticks();
    pg_end();
    return EXIT_SUCCESS;
}

