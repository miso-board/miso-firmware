/**
  ******************************************************************************
  * @file    mesh.c
  * @brief   Multi-board topology, event forwarding and colour routing.
  *          See mesh.h for the geometry and the shape of the protocol.
  ******************************************************************************
  */

#include "mesh.h"

#include "main.h"

#include <string.h>
#include <stdio.h>

/* --- Tuning ---------------------------------------------------------------- */

#define MESH_ANNOUNCE_MS        250u   /* tree beacon, root -> leaves */
#define MESH_STATE_MS           100u   /* held-key sync, leaves -> root */
#define MESH_BOARD_TIMEOUT_MS   500u   /* no KEYSTATE this long = board gone */

#define MESH_NO_PARENT          LINK_PORT_COUNT
#define MESH_DEPTH_NONE         0xFFu

/* Wire payload sizes. All little-endian, packed by hand rather than by struct
 * so alignment and padding can never bite. */
#define ANN_LEN     17   /* root32 | src32 | parent32 | ox16 | oy16 | depth8 */
#define KEYEV_LEN   16   /* origin32 | seq8 | sensor8 | x16 | y16 | kind8 | vel8 | dt32 */
#define KST_LEN     12   /* origin32 | ox16 | oy16 | held32 */
#define COLOR_LEN   (4 + MESH_RGB_BYTES)   /* dx16 | dy16 | rgb[93] */

/* --- Geometry -------------------------------------------------------------- */

/* Vector from this board to the neighbour attached at each port. The lattice
 * these generate has determinant 31; see mesh.h. */
static const int8_t dir_vec[LINK_PORT_COUNT][2] = {
  /* TOP    */ {  3, -5 },
  /* BOTTOM */ { -3,  5 },
  /* LEFT   */ { -5, -2 },
  /* RIGHT  */ {  5,  2 },
};

static link_port_t opposite(link_port_t p)
{
  switch (p) {
  case LINK_PORT_TOP:    return LINK_PORT_BOTTOM;
  case LINK_PORT_BOTTOM: return LINK_PORT_TOP;
  case LINK_PORT_LEFT:   return LINK_PORT_RIGHT;
  default:               return LINK_PORT_LEFT;
  }
}

/* --- State ----------------------------------------------------------------- */

typedef enum { ROLE_NONE = 0, ROLE_PARENT, ROLE_CHILD, ROLE_PEER } port_role_t;

static const char *const role_name[] = { "none", "parent", "child", "peer" };

static struct {
  port_role_t role;
  uint32_t    peer_uid;
  uint8_t     geom_ok;   /* peer's port is the opposite edge, as it must be */
} portst[LINK_PORT_COUNT];

static uint32_t    my_uid, root_uid, parent_uid;
static int16_t     my_ox, my_oy;
static uint8_t     my_depth = MESH_DEPTH_NONE;
static link_port_t parent_port = MESH_NO_PARENT;
static uint8_t     is_master;
static uint8_t     key_seq;

/* Master-side view of the grid. `held` is what the master BELIEVES is held,
 * advanced by KEYEV and corrected by KEYSTATE. */
typedef struct {
  uint8_t  used;
  uint32_t uid;
  int16_t  ox, oy;
  uint32_t held;
  uint32_t last_seen;
  uint8_t  last_seq, have_seq;
} board_t;

static board_t  boards[MESH_MAX_BOARDS];
static uint32_t t_announce, t_state, now_tick;
static uint32_t n_missed_down, n_fixed_up, n_geom_err, n_multi_master, n_lost_release;

/* --- Little-endian packing -------------------------------------------------- */

static void put16(uint8_t *b, int16_t v) { b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *b, uint32_t v)
{
  b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
  b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static int16_t  get16(const uint8_t *b) { return (int16_t)((uint16_t)b[0] | ((uint16_t)b[1] << 8)); }
static uint32_t get32(const uint8_t *b)
{
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static uint8_t popcount32(uint32_t v)
{
  uint8_t n = 0;
  while (v) { v &= v - 1u; n++; }
  return n;
}

/* --- Board table (master only) ---------------------------------------------- */

static board_t *board_find(uint32_t uid)
{
  for (uint8_t i = 0; i < MESH_MAX_BOARDS; i++) {
    if (boards[i].used && boards[i].uid == uid) return &boards[i];
  }
  return NULL;
}

static board_t *board_get(uint32_t uid)
{
  board_t *b = board_find(uid);
  if (b) return b;
  for (uint8_t i = 0; i < MESH_MAX_BOARDS; i++) {
    if (!boards[i].used) {
      memset(&boards[i], 0, sizeof(boards[i]));
      boards[i].used = 1;
      boards[i].uid = uid;
      return &boards[i];
    }
  }
  return NULL;   /* grid larger than MESH_MAX_BOARDS */
}

/* Synthesise a key-up for every key this board is believed to be holding. */
static void board_release(board_t *b)
{
  for (uint8_t s = 0; s < MESH_SENSORS; s++) {
    if (!(b->held & (1u << s))) continue;
    int8_t lx, ly;
    Miso_SensorCoord(s, &lx, &ly);
    Miso_EmitKeyUp(s, (int16_t)(b->ox + lx), (int16_t)(b->oy + ly));
    n_lost_release++;
  }
  b->held = 0;
}

/* The whole point of the 100 ms sync: a key we think is down but the board says
 * is up gets released. The reverse -- held there, never seen here -- means an
 * event was lost; it is counted and logged but never fabricated, because the
 * real velocity is gone and a wrong-sounding note is worse than a logged miss. */
static void board_reconcile(board_t *b, uint32_t reported)
{
  uint32_t stuck  = b->held & ~reported;
  uint32_t missed = reported & ~b->held;

  for (uint8_t s = 0; s < MESH_SENSORS; s++) {
    if (!(stuck & (1u << s))) continue;
    int8_t lx, ly;
    Miso_SensorCoord(s, &lx, &ly);
    Miso_EmitKeyUp(s, (int16_t)(b->ox + lx), (int16_t)(b->oy + ly));
    n_fixed_up++;
  }
  n_missed_down += popcount32(missed);
  b->held = reported;
}

/* --- Topology --------------------------------------------------------------- */

static void topo_reset(void)
{
  root_uid = 0;
  parent_uid = 0;
  parent_port = MESH_NO_PARENT;
  my_ox = my_oy = 0;
  my_depth = MESH_DEPTH_NONE;
  for (uint8_t p = 0; p < LINK_PORT_COUNT; p++) portst[p].role = ROLE_NONE;
}

static void become_master(void)
{
  is_master = 1;
  root_uid = my_uid;
  parent_uid = 0;
  parent_port = MESH_NO_PARENT;
  my_ox = my_oy = 0;
  my_depth = 0;
}

static void on_announce(link_port_t p, const uint8_t *d, uint8_t len)
{
  if (len < ANN_LEN) return;

  uint32_t r_root   = get32(d);
  uint32_t r_src    = get32(d + 4);
  uint32_t r_parent = get32(d + 8);
  int16_t  r_ox     = get16(d + 12);
  int16_t  r_oy     = get16(d + 14);
  uint8_t  r_depth  = d[16];

  portst[p].peer_uid = r_src;

  /* Connectors enforce orientation, so a right edge must meet a left edge. If
   * it does not, the boards are physically miswired and any offset derived
   * from this edge would be wrong -- report rather than silently accept. */
  uint8_t geom = (link_peer_port(p) == (uint8_t)opposite(p));
  portst[p].geom_ok = geom;
  if (!geom) {
    n_geom_err++;   /* every occurrence: a steadily climbing count is the signal */
    return;         /* no offset may be derived from a miswired edge */
  }

  /* A neighbour naming us as its parent makes this port a child. */
  if (r_parent == my_uid)      portst[p].role = ROLE_CHILD;
  else if (p == parent_port)   portst[p].role = ROLE_PARENT;
  else                         portst[p].role = ROLE_PEER;

  int16_t cand_ox = (int16_t)(r_ox - dir_vec[p][0]);
  int16_t cand_oy = (int16_t)(r_oy - dir_vec[p][1]);

  if (is_master) {
    /* Two USB-connected boards in one grid would form two trees. Surface it. */
    if (r_root != my_uid) n_multi_master++;
    return;
  }

  if (r_depth >= MESH_DEPTH_NONE - 1u) return;
  uint8_t cand_depth = (uint8_t)(r_depth + 1u);

  uint8_t adopt = 0;
  if (parent_port == MESH_NO_PARENT)      adopt = 1;   /* nothing better */
  else if (p == parent_port)              adopt = 1;   /* refresh from parent */
  else if (cand_depth < my_depth)         adopt = 1;   /* shorter route */
  else if (cand_depth == my_depth && r_src < parent_uid) adopt = 1;  /* stable tiebreak */

  if (adopt) {
    parent_port = p;
    parent_uid  = r_src;
    root_uid    = r_root;
    my_ox = cand_ox;
    my_oy = cand_oy;
    my_depth = cand_depth;
    portst[p].role = ROLE_PARENT;
  } else if (my_depth != MESH_DEPTH_NONE &&
             (cand_ox != my_ox || cand_oy != my_oy)) {
    /* A non-tree edge implies an offset too, and in a valid grid it must agree
     * with the one we already hold. Cycles therefore validate the topology for
     * free; disagreement means the grid is not physically consistent. */
    n_geom_err++;
  }
}

/* --- Event forwarding -------------------------------------------------------- */

static void forward_up(link_port_t from, uint8_t type, const uint8_t *d, uint8_t len)
{
  if (parent_port == MESH_NO_PARENT) return;
  if (from == parent_port) return;        /* never bounce back down */
  if (link_state(parent_port) != LINK_UP) return;
  link_send(parent_port, type, d, len);
}

static void on_keyev(link_port_t p, const uint8_t *d, uint8_t len)
{
  if (len < KEYEV_LEN) return;

  if (!is_master) {
    forward_up(p, LINK_MSG_KEYEV, d, len);
    return;
  }

  uint32_t origin = get32(d);
  uint8_t  seq    = d[4];
  uint8_t  sensor = d[5];
  int16_t  x      = get16(d + 6);
  int16_t  y      = get16(d + 8);
  uint8_t  kind   = d[10];
  uint8_t  vel    = d[11];
  uint32_t dt     = get32(d + 12);

  board_t *b = board_get(origin);
  if (b) {
    /* The tree delivers each event once; a duplicate only appears if the tree
     * reconfigured mid-flight. */
    if (b->have_seq && b->last_seq == seq) return;
    b->last_seq = seq;
    b->have_seq = 1;
    b->last_seen = now_tick;
    if (sensor < MESH_SENSORS) {
      /* Recover the board's origin from the event rather than waiting for its
       * first KEYSTATE: absolute coordinate minus the sensor's local one. */
      int8_t lx, ly;
      Miso_SensorCoord(sensor, &lx, &ly);
      b->ox = (int16_t)(x - lx);
      b->oy = (int16_t)(y - ly);
      if (kind) b->held |= (1u << sensor);
      else      b->held &= ~(1u << sensor);
    }
  }

  if (kind) Miso_EmitKeyDown(sensor, x, y, vel, dt);
  else      Miso_EmitKeyUp(sensor, x, y);
}

static void on_keystate(link_port_t p, const uint8_t *d, uint8_t len)
{
  if (len < KST_LEN) return;

  if (!is_master) {
    forward_up(p, LINK_MSG_KEYSTATE, d, len);
    return;
  }

  uint32_t origin = get32(d);
  board_t *b = board_get(origin);
  if (!b) return;
  b->ox = get16(d + 4);
  b->oy = get16(d + 6);
  b->last_seen = now_tick;
  board_reconcile(b, get32(d + 8));
}

static void on_color(link_port_t p, const uint8_t *d, uint8_t len)
{
  if (len < COLOR_LEN) return;

  if (my_depth != MESH_DEPTH_NONE &&
      get16(d) == my_ox && get16(d + 2) == my_oy) {
    Miso_ApplyColors(d + 4);
    return;   /* offsets are unique, so nobody downstream wants it */
  }

  for (link_port_t q = 0; q < LINK_PORT_COUNT; q++) {
    if (q == p || portst[q].role != ROLE_CHILD) continue;
    if (link_state(q) != LINK_UP) continue;
    link_send(q, LINK_MSG_COLOR, d, len);
  }
}

static void mesh_rx(link_port_t p, uint8_t type, const uint8_t *payload, uint8_t len)
{
  switch (type) {
  case LINK_MSG_ANNOUNCE: on_announce(p, payload, len); break;
  case LINK_MSG_KEYEV:    on_keyev(p, payload, len);    break;
  case LINK_MSG_KEYSTATE: on_keystate(p, payload, len); break;
  case LINK_MSG_COLOR:    on_color(p, payload, len);    break;
  default: break;
  }
}

static void mesh_link_down(link_port_t p)
{
  /* A direct neighbour vanishing is known immediately; anything further away is
   * caught by the KEYSTATE timeout instead. */
  if (is_master && portst[p].peer_uid) {
    board_t *b = board_find(portst[p].peer_uid);
    if (b) {
      board_release(b);
      b->used = 0;
    }
  }
  portst[p].role = ROLE_NONE;
  portst[p].peer_uid = 0;
  portst[p].geom_ok = 0;

  if (!is_master && p == parent_port) topo_reset();
}

/* --- Public API --------------------------------------------------------------- */

void mesh_init(void)
{
  my_uid = HAL_GetUIDw0();
  memset(boards, 0, sizeof(boards));
  memset(portst, 0, sizeof(portst));
  topo_reset();
  link_set_rx_handler(mesh_rx);
  link_set_down_handler(mesh_link_down);
}

void mesh_key_down(uint8_t sensor, int16_t x, int16_t y, uint8_t vel, uint32_t dt_us)
{
  if (is_master) { Miso_EmitKeyDown(sensor, x, y, vel, dt_us); return; }
  if (parent_port == MESH_NO_PARENT || link_state(parent_port) != LINK_UP) return;

  uint8_t d[KEYEV_LEN];
  put32(d, my_uid);
  d[4] = key_seq++;
  d[5] = sensor;
  put16(d + 6, x);
  put16(d + 8, y);
  d[10] = 1;
  d[11] = vel;
  put32(d + 12, dt_us);
  link_send(parent_port, LINK_MSG_KEYEV, d, sizeof(d));
}

void mesh_key_up(uint8_t sensor, int16_t x, int16_t y)
{
  if (is_master) { Miso_EmitKeyUp(sensor, x, y); return; }
  if (parent_port == MESH_NO_PARENT || link_state(parent_port) != LINK_UP) return;

  uint8_t d[KEYEV_LEN];
  put32(d, my_uid);
  d[4] = key_seq++;
  d[5] = sensor;
  put16(d + 6, x);
  put16(d + 8, y);
  d[10] = 0;
  d[11] = 0;
  put32(d + 12, 0);
  link_send(parent_port, LINK_MSG_KEYEV, d, sizeof(d));
}

void mesh_set_colors(int16_t x, int16_t y, const uint8_t *rgb)
{
  if (my_depth != MESH_DEPTH_NONE && x == my_ox && y == my_oy) {
    Miso_ApplyColors(rgb);
    return;
  }
  uint8_t d[COLOR_LEN];
  put16(d, x);
  put16(d + 2, y);
  memcpy(d + 4, rgb, MESH_RGB_BYTES);
  for (link_port_t q = 0; q < LINK_PORT_COUNT; q++) {
    if (portst[q].role != ROLE_CHILD || link_state(q) != LINK_UP) continue;
    link_send(q, LINK_MSG_COLOR, d, sizeof(d));
  }
}

int16_t mesh_offset_x(void) { return my_ox; }
int16_t mesh_offset_y(void) { return my_oy; }
uint8_t mesh_is_master(void) { return is_master; }

void mesh_tick(uint32_t tick)
{
  now_tick = tick;

  /* USB enumerates well after boot, so mastership is decided here, not in init. */
  if (!is_master && link_self_has_usb()) become_master();

  /* Ports whose link dropped without a teardown callback (never came up, or the
   * peer changed) must not keep a stale role. */
  for (link_port_t p = 0; p < LINK_PORT_COUNT; p++) {
    if (link_state(p) != LINK_UP && portst[p].role != ROLE_NONE) {
      portst[p].role = ROLE_NONE;
      portst[p].peer_uid = 0;
    }
  }
  if (!is_master && parent_port != MESH_NO_PARENT &&
      link_state(parent_port) != LINK_UP) {
    topo_reset();
  }

  /* Beacon down the tree. Only boards with a known place in the grid speak. */
  if (my_depth != MESH_DEPTH_NONE && (tick - t_announce) >= MESH_ANNOUNCE_MS) {
    t_announce = tick;
    uint8_t d[ANN_LEN];
    put32(d, root_uid);
    put32(d + 4, my_uid);
    put32(d + 8, parent_uid);
    put16(d + 12, my_ox);
    put16(d + 14, my_oy);
    d[16] = my_depth;
    for (link_port_t p = 0; p < LINK_PORT_COUNT; p++) {
      if (link_state(p) == LINK_UP) link_send(p, LINK_MSG_ANNOUNCE, d, sizeof(d));
    }
  }

  if ((tick - t_state) >= MESH_STATE_MS) {
    t_state = tick;
    if (is_master) {
      /* Keep our own row in the table fresh so the dump is uniform. */
      board_t *self = board_get(my_uid);
      if (self) {
        self->ox = 0; self->oy = 0;
        self->held = Miso_LocalHeldMask();
        self->last_seen = tick;
      }
      /* Anything that stopped reporting is gone -- release its notes. This is
       * what catches boards beyond the immediate neighbours. */
      for (uint8_t i = 0; i < MESH_MAX_BOARDS; i++) {
        if (!boards[i].used || boards[i].uid == my_uid) continue;
        if ((tick - boards[i].last_seen) < MESH_BOARD_TIMEOUT_MS) continue;
        board_release(&boards[i]);
        boards[i].used = 0;
      }
    } else if (my_depth != MESH_DEPTH_NONE && parent_port != MESH_NO_PARENT &&
               link_state(parent_port) == LINK_UP) {
      uint8_t d[KST_LEN];
      put32(d, my_uid);
      put16(d + 4, my_ox);
      put16(d + 6, my_oy);
      put32(d + 8, Miso_LocalHeldMask());
      link_send(parent_port, LINK_MSG_KEYSTATE, d, sizeof(d));
    }
  }
}

void mesh_dump(void)
{
  char line[128];

  snprintf(line, sizeof(line),
           "MESH self=%08lX off=%d,%d depth=%d master=%u root=%08lX\r\n",
           (unsigned long)my_uid, my_ox, my_oy,
           (my_depth == MESH_DEPTH_NONE) ? -1 : (int)my_depth,
           is_master, (unsigned long)root_uid);
  Miso_SendText(line);

  for (link_port_t p = 0; p < LINK_PORT_COUNT; p++) {
    if (link_state(p) != LINK_UP && portst[p].role == ROLE_NONE) continue;
    snprintf(line, sizeof(line), "MESH port=%-6s role=%-6s peer=%08lX geom=%s\r\n",
             link_port_name(p), role_name[portst[p].role],
             (unsigned long)portst[p].peer_uid,
             portst[p].geom_ok ? "ok" : "BAD");
    Miso_SendText(line);
  }

  for (uint8_t i = 0; i < MESH_MAX_BOARDS; i++) {
    if (!boards[i].used) continue;
    snprintf(line, sizeof(line),
             "MESH board=%08lX off=%d,%d held=%08lX age=%lu\r\n",
             (unsigned long)boards[i].uid, boards[i].ox, boards[i].oy,
             (unsigned long)boards[i].held,
             (unsigned long)(now_tick - boards[i].last_seen));
    Miso_SendText(line);
  }

  snprintf(line, sizeof(line),
           "MESH stat missed_down=%lu fixed_up=%lu lost_release=%lu geom_err=%lu multi_master=%lu\r\n",
           (unsigned long)n_missed_down, (unsigned long)n_fixed_up,
           (unsigned long)n_lost_release,
           (unsigned long)n_geom_err, (unsigned long)n_multi_master);
  Miso_SendText(line);
}
