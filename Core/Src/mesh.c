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
#define MESH_COLORGEN_MS        1000u  /* colour-generator beacon, parent -> children */

#define MESH_NO_PARENT          LINK_PORT_COUNT
#define MESH_DEPTH_NONE         0xFFu

/* Wire payload sizes. All little-endian, packed by hand rather than by struct
 * so alignment and padding can never bite. */
#define ANN_LEN     17   /* root32 | src32 | parent32 | ox16 | oy16 | depth8 */
#define KEYEV_LEN   16   /* origin32 | seq8 | sensor8 | x16 | y16 | kind8 | vel8 | dt32 */
#define KST_LEN     12   /* origin32 | ox16 | oy16 | held32 */
#define KST_LEN_V2  13   /* ... | cgen8: the colour-generator seq this board holds,
                          * or CGEN_SEQ_NONE. Appended, and the length check on
                          * receive still accepts KST_LEN, so a pre-0.8.0 board
                          * keeps working and simply never reports one. */
/* Two distinct answers, because they mean opposite things when a colour push
 * looks like it did not arrive. SILENT means the board never reported at all, so
 * it predates 0.8.0 and cannot receive a generator; NONE means it is new enough
 * to report and is telling us it has no generator yet, which points at the
 * propagation path instead. Both are skipped when a seq is bumped. */
#define CGEN_SEQ_NONE   0xFEu   /* reported: "I have no generator" */
#define CGEN_SEQ_SILENT 0xFFu   /* not reported: pre-0.8.0 board */
#define COLOR_LEN   (4 + MESH_RGB_BYTES)   /* dx16 | dy16 | rgb[93] */
#define CGEN_LEN    (1 + MESH_CGEN_BYTES)  /* seq8 | params[40] */

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
  uint8_t  cgen;     /* the generator seq it reports, or one of the two markers
                      * above. Compare against ours to see at a glance whether a
                      * colour push actually reached it. */
} board_t;

static board_t  boards[MESH_MAX_BOARDS];

/* The colour generator this board currently holds, and the sequence number it
 * came with. Held here rather than in main.c because this layer is what re-sends
 * it, and because the seq is a property of the distribution, not of the colours.
 *
 * cgen_seq is only meaningful alongside cgen_have: a board with no set holds no
 * seq, so a beacon carrying seq 0 is still new to it. */
static uint8_t  cgen_params[MESH_CGEN_BYTES];
static uint8_t  cgen_seq;
static uint8_t  cgen_have;
static int16_t  cgen_ox, cgen_oy;   /* the offset we last painted at */
static uint8_t  cgen_painted;

static void cgen_apply(void);            /* defined below; on_announce uses both */
static void cgen_send_port(link_port_t p);

static uint32_t t_announce, t_state, t_cgen, now_tick;
static uint32_t n_missed_down, n_fixed_up, n_geom_err, n_multi_master, n_lost_release;
/* n_ev_nolink: a key event with nowhere to go (no parent, or the parent port not
 * UP) -- it was silently discarded before. n_ev_txfull: link_send refused it,
 * which for a KEYEV means both redundant copies were refused, since one making
 * it through is enough. n_ev_dup: the second copy arriving intact and being
 * suppressed, i.e. the redundancy doing nothing because nothing was lost -- so
 * this one is expected to be large and is not a fault. */
static uint32_t n_ev_nolink, n_ev_txfull, n_ev_dup;

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
      boards[i].cgen = CGEN_SEQ_SILENT;   /* not "seq 0": we have not heard yet */
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

  /* Start beaconing immediately, from our own compiled-in generator, rather than
   * waiting for a 'K' that may never come. Two things follow. A child gets the
   * parameters the moment it is adopted, so it leaves its boot wait early instead
   * of sitting dark for the whole ceiling. And the grid provably runs ONE
   * generator -- the master's -- rather than relying on every board having the
   * same default compiled in, which stops being true the moment two firmware
   * versions are mixed in one grid. */
  if (!cgen_have) {
    Miso_PackColorGen(cgen_params);
    cgen_seq = 1;
    cgen_have = 1;
    cgen_ox = 0;
    cgen_oy = 0;
    cgen_painted = 1;
  }
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
  port_role_t was = portst[p].role;
  if (r_parent == my_uid)      portst[p].role = ROLE_CHILD;
  else if (p == parent_port)   portst[p].role = ROLE_PARENT;
  else                         portst[p].role = ROLE_PEER;

  /* A board that has just adopted us is dark, waiting for exactly this. Sending
   * it here rather than leaving it to the next beacon is what keeps a hot-plugged
   * board inside its boot wait, so it ripples up into the right colours instead
   * of into the compiled-in default. */
  if (portst[p].role == ROLE_CHILD && was != ROLE_CHILD) cgen_send_port(p);

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

    /* Our colours are a function of where we are, so MOVING invalidates them
     * even though the parameters have not changed: unplugged from one edge of the
     * grid and replugged on another, the master's seq is untouched and would
     * never repaint us.
     *
     * Against the offset we last painted at, not against the one we just had, so
     * that a link that dropped and came back in the same place repaints nothing
     * -- and a board hand-painted by 'L' in the meantime keeps its paint. */
    if (cgen_have && (!cgen_painted || cand_ox != cgen_ox || cand_oy != cgen_oy)) {
      cgen_apply();
    }
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
    if (b->have_seq && b->last_seq == seq) { n_ev_dup++; return; }
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
  b->cgen = (len >= KST_LEN_V2) ? d[12] : CGEN_SEQ_SILENT;
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

/* Paint from the held generator, remembering where we were when we did -- but
 * only if it actually took, so a set main.c refuses cannot leave us believing we
 * are painted and skipping the repaint on a later move. */
static void cgen_apply(void)
{
  if (!Miso_ApplyColorGen(cgen_params)) return;
  cgen_ox = my_ox;
  cgen_oy = my_oy;
  cgen_painted = 1;
}

static void cgen_send_port(link_port_t p)
{
  if (!cgen_have || link_state(p) != LINK_UP) return;

  uint8_t d[CGEN_LEN];
  d[0] = cgen_seq;
  memcpy(d + 1, cgen_params, MESH_CGEN_BYTES);
  link_send(p, LINK_MSG_COLORGEN, d, sizeof(d));
}

/* Beacon the held generator to every child. Costs 47 bytes on the wire per port
 * -- ~1 ms of a 460800 link once a second -- which is why it can simply repeat
 * instead of carrying an ack. */
static void cgen_send_children(void)
{
  for (link_port_t q = 0; q < LINK_PORT_COUNT; q++) {
    if (portst[q].role != ROLE_CHILD) continue;
    cgen_send_port(q);
  }
}

static void on_colorgen(link_port_t p, const uint8_t *d, uint8_t len)
{
  if (len < CGEN_LEN) return;
  /* Colours flow down only. Taking this from anywhere else would let a cycle in
   * the grid hand us a set that is on its way somewhere, or loop one back. */
  if (p != parent_port) return;

  /* Apply when the PARAMETERS differ, or when the seq does. Both, because they
   * answer different questions, and either alone is wrong:
   *
   *   - content differs -> new colours, apply. This is what makes the seq's
   *     8-bit wraparound harmless. It is reachable: the companion debounces a
   *     dragged slider at 100 ms, so ~26 s of dragging is 256 pushes, and a child
   *     unplugged across exactly that many would otherwise see its own seq come
   *     back around and ignore a genuinely different set.
   *   - seq differs -> something was pushed even though it is the same set, which
   *     is how a board hand-painted by 'L' is pulled back onto the generator.
   *
   * Neither -> the periodic beacon, which must NOT repaint: that is what leaves
   * an 'L' applied since the last push alone. */
  uint8_t seq = d[0];
  if (cgen_have && seq == cgen_seq &&
      memcmp(cgen_params, d + 1, MESH_CGEN_BYTES) == 0) {
    return;
  }
  memcpy(cgen_params, d + 1, MESH_CGEN_BYTES);
  cgen_seq  = seq;
  cgen_have = 1;
  cgen_apply();

  /* Straight on down, so a deep grid repaints in one hop rather than one beacon
   * period per level. The periodic beacon is the backstop, not the fast path. */
  cgen_send_children();
}

static void mesh_rx(link_port_t p, uint8_t type, const uint8_t *payload, uint8_t len)
{
  switch (type) {
  case LINK_MSG_ANNOUNCE: on_announce(p, payload, len); break;
  case LINK_MSG_KEYEV:    on_keyev(p, payload, len);    break;
  case LINK_MSG_KEYSTATE: on_keystate(p, payload, len); break;
  case LINK_MSG_COLOR:    on_color(p, payload, len);    break;
  case LINK_MSG_COLORGEN: on_colorgen(p, payload, len); break;
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

/* Send one key event toward the master, twice.
 *
 * A key event is the one frame on this link whose loss is audible and
 * unrecoverable. Everything else here is periodic and self-healing: a lost PING
 * is replaced 50 ms later, a lost ANNOUNCE 250 ms later, a lost COLORGEN beacon
 * a second later. A lost KEYEV is a dead note, permanently -- board_reconcile()
 * will notice and count it in missed_down but deliberately will not fabricate
 * the note, because the velocity is gone.
 *
 * Measured on a three-board chain, the master's right port showed
 * `err=698 (uart=375 ... sum=323)` with `rxfull=0 txfull=0` -- around a 3% frame
 * error rate, entirely from corruption rather than from any queue we overflowed,
 * and `missed_down=151` dead notes to go with it. At that rate a single copy
 * loses a note every second or two of hard trilling, which is what a player
 * feels as a dead key.
 *
 * Sending the frame twice takes that to ~0.1%, and it needs nothing new on the
 * receiving side: the duplicate suppression in on_keyev() already discards a
 * repeat of the previous seq, and link_send is FIFO so the two copies stay
 * adjacent. Cost is 21 extra bytes per event against a 46 kB/s link -- less than
 * the colour beacon already spends every second.
 *
 * This is a mitigation, not a cure. The durable fix for a connector at 3% is the
 * series resistors in the README's hardware note; this just stops a marginal
 * connector from being something you can hear. */
static void keyev_send(uint8_t sensor, int16_t x, int16_t y,
                       uint8_t kind, uint8_t vel, uint32_t dt_us)
{
  if (parent_port == MESH_NO_PARENT || link_state(parent_port) != LINK_UP) {
    n_ev_nolink++;
    return;
  }

  uint8_t d[KEYEV_LEN];
  put32(d, my_uid);
  d[4] = key_seq++;
  d[5] = sensor;
  put16(d + 6, x);
  put16(d + 8, y);
  d[10] = kind;
  d[11] = vel;
  put32(d + 12, dt_us);

  /* Both copies carry the same seq, which is what makes the second one free to
   * discard. Only a failure of both is a lost event worth counting. */
  int a = link_send(parent_port, LINK_MSG_KEYEV, d, sizeof(d));
  int b = link_send(parent_port, LINK_MSG_KEYEV, d, sizeof(d));
  if (!a && !b) n_ev_txfull++;
}

void mesh_key_down(uint8_t sensor, int16_t x, int16_t y, uint8_t vel, uint32_t dt_us)
{
  if (is_master) { Miso_EmitKeyDown(sensor, x, y, vel, dt_us); return; }
  keyev_send(sensor, x, y, 1, vel, dt_us);
}

void mesh_key_up(uint8_t sensor, int16_t x, int16_t y)
{
  if (is_master) { Miso_EmitKeyUp(sensor, x, y); return; }
  keyev_send(sensor, x, y, 0, 0, 0);
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

void mesh_set_colorgen(const uint8_t *params)
{
  memcpy(cgen_params, params, MESH_CGEN_BYTES);
  /* Bump unconditionally, even for an identical set: the seq is what tells a
   * child that something was pushed, and a board hand-painted since needs to be
   * brought back onto the generator by a re-push of the same parameters. */
  cgen_seq++;
  /* Both markers are reserved values in the KEYSTATE byte. */
  while (cgen_seq == CGEN_SEQ_NONE || cgen_seq == CGEN_SEQ_SILENT) cgen_seq++;
  cgen_have = 1;
  /* main.c has already applied and rendered these; just record where, so the
   * move check in on_announce() has a baseline (the master is always (0,0)). */
  cgen_ox = my_ox;
  cgen_oy = my_oy;
  cgen_painted = 1;
  cgen_send_children();
}

uint8_t mesh_has_colorgen(void) { return cgen_have; }
uint8_t mesh_colorgen_seq(void) { return cgen_seq; }

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

  /* Re-beacon the colour generator down the tree. Idempotent by the seq check on
   * the far side, so this covers a dropped frame and a child that has only just
   * come up, with nothing tracking who has been told. */
  if (cgen_have && (tick - t_cgen) >= MESH_COLORGEN_MS) {
    t_cgen = tick;
    cgen_send_children();
  }

  if ((tick - t_state) >= MESH_STATE_MS) {
    t_state = tick;
    if (is_master) {
      /* Keep our own row in the table fresh so the dump is uniform. */
      board_t *self = board_get(my_uid);
      if (self) {
        self->ox = 0; self->oy = 0;
        self->held = Miso_LocalHeldMask();
        self->cgen = cgen_have ? cgen_seq : CGEN_SEQ_NONE;
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
      uint8_t d[KST_LEN_V2];
      put32(d, my_uid);
      put16(d + 4, my_ox);
      put16(d + 6, my_oy);
      put32(d + 8, Miso_LocalHeldMask());
      d[12] = cgen_have ? cgen_seq : CGEN_SEQ_NONE;
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
    char cg[8];
    if (boards[i].cgen == CGEN_SEQ_SILENT)    snprintf(cg, sizeof(cg), "old");
    else if (boards[i].cgen == CGEN_SEQ_NONE) snprintf(cg, sizeof(cg), "none");
    else snprintf(cg, sizeof(cg), "%u", boards[i].cgen);
    snprintf(line, sizeof(line),
             "MESH board=%08lX off=%d,%d held=%08lX cgen=%s age=%lu\r\n",
             (unsigned long)boards[i].uid, boards[i].ox, boards[i].oy,
             (unsigned long)boards[i].held, cg,
             (unsigned long)(now_tick - boards[i].last_seen));
    Miso_SendText(line);
  }

  /* Before the stat line, which the companion uses as the commit point for a
   * topology snapshot. */
  if (cgen_have) {
    snprintf(line, sizeof(line),
             "MESH cgen seq=%u M=%d,%d,%d,%d anchor=%d,%d start=%d n=%u flags=%u bright=%u\r\n",
             cgen_seq,
             get16(cgen_params), get16(cgen_params + 2),
             get16(cgen_params + 4), get16(cgen_params + 6),
             get16(cgen_params + 8), get16(cgen_params + 10),
             (int)(int8_t)cgen_params[12], cgen_params[13],
             cgen_params[14], cgen_params[15]);
  } else {
    snprintf(line, sizeof(line), "MESH cgen none\r\n");
  }
  Miso_SendText(line);

  snprintf(line, sizeof(line),
           "MESH stat missed_down=%lu fixed_up=%lu lost_release=%lu geom_err=%lu multi_master=%lu\r\n",
           (unsigned long)n_missed_down, (unsigned long)n_fixed_up,
           (unsigned long)n_lost_release,
           (unsigned long)n_geom_err, (unsigned long)n_multi_master);
  Miso_SendText(line);

  /* evdup is the redundancy working and is expected to be roughly the event
   * count; it falling well below that is the interesting case, because it means
   * first copies are arriving where second ones did not. */
  snprintf(line, sizeof(line),
           "MESH ev dup=%lu nolink=%lu txfull=%lu\r\n",
           (unsigned long)n_ev_dup, (unsigned long)n_ev_nolink,
           (unsigned long)n_ev_txfull);
  Miso_SendText(line);
}
