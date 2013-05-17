/*********************************************************************
PicoTCP. Copyright (c) 2012 TASS Belgium NV. Some rights reserved.
See LICENSE and COPYING for usage.

RFC 1112, 2236, 3376, 3678, 4607

Authors: Simon Maes, Brecht Van Cauwenberghe, Kristof Roelants
*********************************************************************/

#include "pico_stack.h"
#include "pico_ipv4.h"
#include "pico_igmp.h"
#include "pico_config.h"
#include "pico_eth.h"
#include "pico_addressing.h"
#include "pico_frame.h"
#include "pico_tree.h"
#include "pico_device.h"

/* membership states */
#define IGMP_STATE_NON_MEMBER            (0x0)
#define IGMP_STATE_DELAYING_MEMBER       (0x1)
#define IGMP_STATE_IDLE_MEMBER           (0x2)

/* events */ 
#define IGMP_EVENT_LEAVE_GROUP           (0x0)
#define IGMP_EVENT_JOIN_GROUP            (0x1)
#define IGMP_EVENT_QUERY_RECV            (0x2)
#define IGMP_EVENT_REPORT_RECV           (0x3)
#define IGMP_EVENT_TIMER_EXPIRED         (0x4)

/* message types */
#define IGMP_TYPE_MEM_QUERY              (0x11)
#define IGMP_TYPE_V1_MEM_REPORT          (0x12)
#define IGMP_TYPE_V2_MEM_REPORT          (0x16)
#define IGMP_TYPE_LEAVE_GROUP            (0x17)
#define IGMP_TYPE_V3_MEM_REPORT          (0x22)

/* group record types */
#define IGMP_MODE_IS_INCLUDE             (1)
#define IGMP_MODE_IS_EXCLUDE             (2)
#define IGMP_CHANGE_TO_INCLUDE_MODE      (3)
#define IGMP_CHANGE_TO_EXCLUDE_MODE      (4)
#define IGMP_ALLOW_NEW_SOURCES           (5)
#define IGMP_BLOCK_OLD_SOURCES           (6)

/* host flag */
#define IGMP_HOST_LAST                   (0x1)
#define IGMP_HOST_NOT_LAST               (0x0)

/* misc */
#define NO_ACTIVE_TIMER                  (0)
#define IP_OPTION_ROUTER_ALERT_LEN       (4)
#define IGMP_DEFAULT_MAX_RESPONSE_TIME   (100)
#define IGMP_UNSOLICITED_REPORT_INTERVAL (100)
#define IGMP_ALL_HOST_GROUP              long_be(0xE0000001) /* 224.0.0.1 */
#define IGMP_ALL_ROUTER_GROUP            long_be(0xE0000002) /* 224.0.0.2 */
#define IGMPV3_ALL_ROUTER_GROUP          long_be(0xE0000016) /* 224.0.0.22 */

struct __attribute__((packed)) igmpv2_message {
  uint8_t type;
  uint8_t max_resp_time;
  uint16_t crc;
  uint32_t mcast_addr;
};

struct __attribute__((packed)) igmpv3_query {
  uint8_t type;
  uint8_t max_resp_time;
  uint16_t crc;
  uint32_t mcast_addr;
  uint8_t rsq;
  uint8_t qqic;
  uint16_t sources;
  uint32_t source_addr[0];
};

struct __attribute__((packed)) igmpv3_group_record {
  uint8_t type;
  uint8_t aux;
  uint16_t sources;
  uint32_t mcast_addr;
  uint32_t source_addr[0];
};

struct __attribute__((packed)) igmpv3_report {
  uint8_t type;
  uint8_t res0;
  uint16_t crc;
  uint16_t res1;
  uint16_t groups;
  struct igmpv3_group_record record[0];
};

struct igmp_packet_params {
  uint8_t event;
  uint8_t max_resp_time;
  /* uint16_t checksum */
  struct pico_ip4 mcast_addr;
  struct pico_ip4 src_interface;
  struct pico_frame *f;
  unsigned long timer_starttime;
};

/*================= RB_TREE FUNCTIONALITY ================*/

struct mgroup_info {
  struct pico_ip4 mgroup_addr;
  struct pico_ip4 src_interface;
  unsigned long active_timer_starttime;
  /* Connector for trees */
  uint16_t delay;
  uint8_t membership_state;
  uint8_t Last_Host_flag;
};

struct timer_callback_info {
  unsigned long timer_starttime;
  struct pico_frame *f;
};

static int mgroup_cmp(void *ka,void *kb)
{
	struct mgroup_info *a=ka, *b=kb;
  if (a->mgroup_addr.addr < b->mgroup_addr.addr) {
    return -1;
  }
  else if (a->mgroup_addr.addr > b->mgroup_addr.addr) {
    return 1;
  }
  else {
     /* a and b are identical */
    return 0;
  }
}

PICO_TREE_DECLARE(KEYTable,mgroup_cmp);

static struct mgroup_info *pico_igmp_find_mgroup(struct pico_ip4 *mgroup_addr)
{
  struct mgroup_info test = {{0}};
  test.mgroup_addr.addr = mgroup_addr->addr;
  /* returns NULL if test can not be found */
  return pico_tree_findKey(&KEYTable,&test);
}

static int pico_igmp_del_mgroup(struct mgroup_info *info)
{
  if(!info){
    pico_err = PICO_ERR_EINVAL;
    return -1;
  }
  else {
    // RB_REMOVE returns pointer to removed element, NULL to indicate error·
    if(pico_tree_delete(&KEYTable,info))
      pico_free(info);
    else {
      pico_err = PICO_ERR_EEXIST;
      return -1;// Do not free, error on removing element from tree
    }
  }
  return 0;
}

/*========================================================*/

static int pico_igmp_process_event(struct igmp_packet_params *params);
static void generate_event_timer_expired(long unsigned int empty, void *info);

#ifdef PICO_UNIT_TEST_IGMP
#define igmp_dbg dbg
static int pico_igmp_process_event(struct igmp_packet_params *params);
static int pico_igmp_analyse_packet(struct pico_frame *f, struct igmp_packet_params *params);
static int pico_igmp_process_in(struct pico_protocol *self, struct pico_frame *f);


int test_pico_igmp_process_in(struct pico_protocol *self, struct pico_frame *f){
  pico_igmp_process_in(self, f);
  return 0;
}
int test_pico_igmp_set_membershipState(struct pico_ip4 *mgroup_addr ,uint8_t state){
  struct mgroup_info *info = pico_igmp_find_mgroup(mgroup_addr);
  info->membership_state = state;
  igmp_dbg("DEBUG_IGMP:STATE = %s\n", (info->membership_state == 0 ? "Non-Member" : (info->membership_state == 1 ? "Delaying MEMBER" : "Idle MEMBER"))); 
  return 0;
}
uint8_t test_pico_igmp_get_membershipState(struct pico_ip4 *mgroup_addr){
  struct mgroup_info *info = pico_igmp_find_mgroup(mgroup_addr);
  igmp_dbg("DEBUG_IGMP:STATE = %s\n", (info->membership_state == 0 ? "Non-Member" : (info->membership_state == 1 ? "Delaying Member" : "Idle Member"))); 
  return info->membership_state;
}
int test_pico_igmp_process_event(struct igmp_packet_params *params) {
   pico_igmp_process_event(params);
   return 0;
}

int test_pico_igmp_analyse_packet(struct pico_frame *f, struct igmp_packet_params *params){
  pico_igmp_analyse_packet(f, params);
  return 0;
}
#else
#define igmp_dbg(...) do{}while(0)
#endif


/* Queues */
static struct pico_queue igmp_in = {};
static struct pico_queue igmp_out = {};

static int pico_igmp_analyse_packet(struct pico_frame *f, struct igmp_packet_params *params)
{
  struct igmpv2_message *hdr = (struct igmpv2_message *) f->transport_hdr;
  switch (hdr->type) {
    case IGMP_TYPE_MEM_QUERY:
       params->event = IGMP_EVENT_QUERY_RECV;
       break;
    case IGMP_TYPE_V1_MEM_REPORT:
       params->event = IGMP_EVENT_REPORT_RECV;
       break;
    case IGMP_TYPE_V2_MEM_REPORT:
       params->event = IGMP_EVENT_REPORT_RECV;
       break;
    default:
       pico_frame_discard(f);
       pico_err = PICO_ERR_EINVAL;
       return -1;
  }
  params->mcast_addr.addr = hdr->mcast_addr;
  params->max_resp_time = hdr->max_resp_time;
  params->f = f;
  return 0;
}

static int check_igmp_checksum(struct pico_frame *f)
{
  struct igmpv2_message *igmp_hdr = (struct igmpv2_message *) f->transport_hdr;
  uint16_t header_checksum;

  if (!igmp_hdr) {
    pico_err = PICO_ERR_EINVAL;
    return -1;
  }
  header_checksum = igmp_hdr->crc;
  igmp_hdr->crc=0;

  if (header_checksum == short_be(pico_checksum(igmp_hdr, sizeof(struct igmpv2_message)))) {
    igmp_hdr->crc = header_checksum;
    return 0;
  } else {
    igmp_hdr->crc = header_checksum;
    pico_err = PICO_ERR_EFAULT;
    return -1;
  }
}

static int pico_igmp_checksum(struct pico_frame *f)
{
  struct igmpv2_message *igmp_hdr = (struct igmpv2_message *) f->transport_hdr;
  if (!igmp_hdr) {
    pico_err = PICO_ERR_EINVAL;
    return -1;
  }
  igmp_hdr->crc = 0;
  igmp_hdr->crc = short_be(pico_checksum(igmp_hdr, sizeof(struct igmpv2_message)));
  return 0;
}

static int pico_igmp_process_in(struct pico_protocol *self, struct pico_frame *f)
{
  struct igmp_packet_params params;
 
  if (check_igmp_checksum(f) == 0) {
    if (!pico_igmp_analyse_packet(f, &params)) {
      pico_igmp_process_event(&params);
    }
  } else {
    igmp_dbg("IGMP: failure on checksum\n");
    pico_frame_discard(f);
  }
  return 0;
}

static int pico_igmp_process_out(struct pico_protocol *self, struct pico_frame *f) {
  // not supported.
  return 0;
}

/* Interface: protocol definition */
struct pico_protocol pico_proto_igmp = {
  .name = "igmp",
  .proto_number = PICO_PROTO_IGMP,
  .layer = PICO_LAYER_TRANSPORT,
  .process_in = pico_igmp_process_in,
  .process_out = pico_igmp_process_out,
  .q_in = &igmp_in,
  .q_out = &igmp_out,
};

int pico_igmp_state_change(struct pico_ip4 *mcast_link, struct pico_ip4 *mcast_group, uint8_t filter_mode, struct pico_tree *MCASTFilter, uint8_t state) 
{
  struct igmp_packet_params params = {0};
  
  if (mcast_group->addr == IGMP_ALL_HOST_GROUP)
    return 0;

  switch (state) {
    case PICO_IGMP_STATE_CREATE:
      /* fall through */

    case PICO_IGMP_STATE_UPDATE:
      params.event = IGMP_EVENT_JOIN_GROUP;
      break;
    
    case PICO_IGMP_STATE_DELETE:
      params.event = IGMP_EVENT_LEAVE_GROUP;
      break;

    default:
      return -1;
  }

  params.mcast_addr = *mcast_group;
  params.src_interface = *mcast_link;

  return pico_igmp_process_event(&params);
}

static int start_timer(struct igmp_packet_params *params,const uint16_t delay)
{
  struct mgroup_info *info = pico_igmp_find_mgroup(&(params->mcast_addr));
  struct timer_callback_info *timer_info= pico_zalloc(sizeof(struct timer_callback_info));

  timer_info->timer_starttime = PICO_TIME_MS();
  timer_info->f = params->f;
  info->delay = delay;
  info->active_timer_starttime = timer_info->timer_starttime;
  pico_timer_add(delay, &generate_event_timer_expired, timer_info);
  return 0;
}

static int stop_timer(struct pico_ip4 *mcast_addr)
{
  struct mgroup_info *info = pico_igmp_find_mgroup(mcast_addr);
  info->active_timer_starttime = NO_ACTIVE_TIMER;
  return 0;
}

static int reset_timer(struct igmp_packet_params *params)
{
  uint8_t ret = 0;
  uint16_t delay = pico_rand() % (params->max_resp_time*100); 

  ret |= stop_timer(&(params->mcast_addr));
  ret |= start_timer(params, delay);
  return ret;
}

static int send_membership_report(struct pico_frame *f)
{
  uint8_t ret = 0;
  struct igmpv2_message *igmp_hdr = (struct igmpv2_message *) f->transport_hdr;
  struct pico_ip4 dst = {0};
  struct pico_ip4 mcast_addr = {0};

  mcast_addr.addr = igmp_hdr->mcast_addr;
  dst.addr = igmp_hdr->mcast_addr;

  igmp_dbg("IGMP: send membership report on group %08X\n", mcast_addr.addr);
  pico_ipv4_frame_push(f, &dst, PICO_PROTO_IGMP);
  ret |= stop_timer(&mcast_addr);
  return ret;
}

static int send_leave(struct pico_frame *f)
{
  uint8_t ret = 0;
  struct igmpv2_message *igmp_hdr = (struct igmpv2_message *) f->transport_hdr;
  struct pico_ip4 mcast_addr = {0};
  struct pico_ip4 dst = {0};

  mcast_addr.addr = igmp_hdr->mcast_addr;
  dst.addr = IGMP_ALL_ROUTER_GROUP;

  igmp_dbg("IGMP: send leave group on group %08X\n", mcast_addr.addr);
  pico_ipv4_frame_push(f,&dst,PICO_PROTO_IGMP);
  ret |= stop_timer(&mcast_addr);
  return ret;
}

static int create_igmp_frame(struct pico_frame **f, struct pico_ip4 src, struct pico_ip4 *mcast_addr, uint8_t type)
{
  uint8_t ret = 0;
  struct igmpv2_message *igmp_hdr = NULL;

  *f = pico_proto_ipv4.alloc(&pico_proto_ipv4, IP_OPTION_ROUTER_ALERT_LEN + sizeof(struct igmpv2_message));
  (*f)->net_len += IP_OPTION_ROUTER_ALERT_LEN;
  (*f)->transport_hdr += IP_OPTION_ROUTER_ALERT_LEN;
  (*f)->transport_len -= IP_OPTION_ROUTER_ALERT_LEN;
  (*f)->len += IP_OPTION_ROUTER_ALERT_LEN;
  (*f)->dev = pico_ipv4_link_find(&src);

  // Fill The IGMP_HDR
  igmp_hdr = (struct igmpv2_message *) (*f)->transport_hdr;

  igmp_hdr->type = type;
  igmp_hdr->max_resp_time = IGMP_DEFAULT_MAX_RESPONSE_TIME;
  igmp_hdr->mcast_addr = mcast_addr->addr;

  ret |= pico_igmp_checksum(*f);
  return ret;
}

/*================== TIMER CALLBACKS ====================*/

static void generate_event_timer_expired(long unsigned int empty, void *data)
{
  struct timer_callback_info *info = (struct timer_callback_info *) data;
  struct igmp_packet_params params = {0};
  struct pico_frame* f = (struct pico_frame*)info->f;
  struct igmpv2_message *igmp_hdr = (struct igmpv2_message *) f->transport_hdr;

  params.event = IGMP_EVENT_TIMER_EXPIRED;
  params.mcast_addr.addr = igmp_hdr->mcast_addr;
  params.timer_starttime = info->timer_starttime;
  params.f = info->f;

  pico_igmp_process_event(&params);
  pico_free(info);  
}

/* ------------------ */
/* HOST STATE MACHINE */
/* ------------------ */

/* state callbacks prototype */
typedef int (*callback)(struct igmp_packet_params *);

/* stop timer, send leave if flag set */
static int stslifs(struct igmp_packet_params *params)
{
  uint8_t ret = 0;
  struct mgroup_info *info = pico_igmp_find_mgroup(&(params->mcast_addr));
  struct pico_frame *f = NULL;

  igmp_dbg("IGMP: event = leave group | action = stop timer, send leave if flag set\n");

  ret |= stop_timer(&(params->mcast_addr));
  if (IGMP_HOST_LAST == info->Last_Host_flag) {
    ret |= create_igmp_frame(&f, params->src_interface, &(params->mcast_addr), IGMP_TYPE_LEAVE_GROUP);
    ret |= send_leave(f);
  }

  if ( 0 == ret) {
    /* delete from tree */
    pico_igmp_del_mgroup(info);
    igmp_dbg("IGMP: new state = non-member\n");
    return 0;
  } else {
    pico_err =  PICO_ERR_EFAULT;
    return -1;
  }
}

/* send report, set flag, start timer */
static int srsfst(struct igmp_packet_params *params)
{
  uint8_t ret = 0;
  struct pico_frame *f = NULL;
  struct mgroup_info *info = pico_zalloc(sizeof(struct mgroup_info));
  struct pico_frame *copy_frame;

  igmp_dbg("IGMP: event = join group | action = send report, set flag, start timer\n");

  info->mgroup_addr = params->mcast_addr;
  info->src_interface = params->src_interface;
  info->membership_state = IGMP_STATE_NON_MEMBER;
  info->Last_Host_flag = IGMP_HOST_LAST;
  info->active_timer_starttime = NO_ACTIVE_TIMER;
  pico_tree_insert(&KEYTable,info);

  ret |= create_igmp_frame(&f, params->src_interface, &(params->mcast_addr), IGMP_TYPE_V2_MEM_REPORT);

  copy_frame = pico_frame_copy(f);
  if (copy_frame == NULL) {
    pico_err = PICO_ERR_EINVAL;
    return -1;
  }
  ret |= send_membership_report(copy_frame);
  info->delay = (pico_rand() % (IGMP_UNSOLICITED_REPORT_INTERVAL * 100)); 
  params->f = f;
  ret |= start_timer(params, info->delay);

  if(0 == ret) {
    struct mgroup_info *info = pico_igmp_find_mgroup(&(params->mcast_addr));
    info->membership_state = IGMP_STATE_DELAYING_MEMBER;
    igmp_dbg("IGMP: new state = delaying member\n");
    return 0;
  } else {
    pico_err = PICO_ERR_EFAULT;
    return -1;
  }
}

/* send leave if flag set */
static int slifs(struct igmp_packet_params *params)
{
  struct pico_frame *f = NULL;
  struct mgroup_info *info;
  uint8_t ret = 0;

  igmp_dbg("IGMP: event = leave group | action = send leave if flag set\n");

  info = pico_igmp_find_mgroup(&(params->mcast_addr));
  if (IGMP_HOST_LAST == info->Last_Host_flag) {
    ret |= create_igmp_frame(&f, params->src_interface, &(params->mcast_addr), IGMP_TYPE_LEAVE_GROUP);
    send_leave(f);
  }

  if (0 == ret) {
    /* delete from tree */
    pico_igmp_del_mgroup(info);
    igmp_dbg("IGMP: new state = non-member\n");
    return 0;
  } else {
    pico_err = PICO_ERR_ENOENT;
    return -1;
  }
}

/* start timer */
static int st(struct igmp_packet_params *params)
{
  uint8_t ret = 0;
  struct mgroup_info *info = pico_igmp_find_mgroup(&(params->mcast_addr));

  igmp_dbg("IGMP: event = query received | action = start timer\n");

  ret |= create_igmp_frame(&(params->f), info->src_interface, &(params->mcast_addr), IGMP_TYPE_V2_MEM_REPORT);
  info->delay = (pico_rand() % (params->max_resp_time*100)); 
  ret |= start_timer(params, info->delay);

  if (0 == ret) {
    info->membership_state = IGMP_STATE_DELAYING_MEMBER;
    igmp_dbg("IGMP: new state = delaying member\n");
    return 0;
  } else {
    pico_err = PICO_ERR_ENOENT;
    return -1;
  }
}

/* stop timer, clear flag */
static int stcl(struct igmp_packet_params *params)
{
  uint8_t ret = 0;
  struct mgroup_info *info = pico_igmp_find_mgroup(&(params->mcast_addr));

  igmp_dbg("IGMP: event = report received | action = stop timer, clear flag\n");

  ret |= stop_timer(&(params->mcast_addr));
  info->Last_Host_flag = IGMP_HOST_NOT_LAST;

  if (0 == ret) {
    info->membership_state = IGMP_STATE_IDLE_MEMBER;
    igmp_dbg("IGMP: new state = idle member\n");
    return 0;
  } else {
    pico_err = PICO_ERR_ENOENT;
    return -1;
  }
}

/* send report, set flag */
static int srsf(struct igmp_packet_params *params)
{
  uint8_t ret = 0;
  struct mgroup_info *info = pico_igmp_find_mgroup(&(params->mcast_addr));

  igmp_dbg("IGMP: event = timer expired | action = send report, set flag\n");

  /* start time of mgroup == start time of expired timer? */
  if (info->active_timer_starttime == params->timer_starttime) {
    ret |= send_membership_report(params->f);
  } else {
    pico_frame_discard(params->f);
  }

  if (0 == ret) {
    info->membership_state = IGMP_STATE_IDLE_MEMBER;
    igmp_dbg("IGMP: new state = idle member\n"); 
    return 0;
  } else {
    pico_err = PICO_ERR_ENOENT;
    return -1;
  }
}

/* reset timer if max response time < current timer */
static int rtimrtct(struct igmp_packet_params *params)
{
  uint8_t ret = 0;
  struct mgroup_info *info = pico_igmp_find_mgroup(&(params->mcast_addr));
  unsigned long current_time_left = ((unsigned long)info->delay - (PICO_TIME_MS() - (unsigned long)info->active_timer_starttime));

  igmp_dbg("IGMP: event = query received | action = reset timer if max response time < current timer\n");

  if (((unsigned long)(params->max_resp_time * 100)) < current_time_left) {
    ret |= create_igmp_frame(&(params->f), params->src_interface, &(params->mcast_addr), IGMP_TYPE_V2_MEM_REPORT);
    ret |= reset_timer(params);
  }

  if (0 == ret) {
    info->membership_state = IGMP_STATE_DELAYING_MEMBER;
    igmp_dbg("IGMP: new state = delaying member\n"); 
    return 0;
  } else {
    pico_err = PICO_ERR_ENOENT;
    return -1;
  }
}

static int discard(struct igmp_packet_params *params){
  igmp_dbg("IGMP: ignore and discard frame\n");
  pico_frame_discard(params->f);
  return 0;
}

static int err_non(struct igmp_packet_params *params){
  igmp_dbg("IGMP ERROR: state = non-member, event = %u\n", params->event);
  pico_err = PICO_ERR_ENOENT;
  return -1;
}

static int err_delaying(struct igmp_packet_params *params){
  igmp_dbg("IGMP ERROR: state = delaying member, event = %u\n", params->event);
  pico_err = PICO_ERR_EEXIST;
  return -1;
}

static int err_idle(struct igmp_packet_params *params){
  igmp_dbg("IGMP ERROR: state = idle member, event = %u\n", params->event);
  pico_err = PICO_ERR_EEXIST;
  return -1;
}

/* finite state machine table */
const callback host_membership_diagram_table[3][5] =
{ /* event                    |Leave Group   |Join Group   |Query Received  |Report Received  |Timer Expired */
/* state Non-Member      */ { err_non,       srsfst,       discard,         err_non,          discard },
/* state Delaying Member */ { stslifs,       err_delaying, rtimrtct,        stcl,             srsf    },
/* state Idle Member     */ { slifs,         err_idle,     st,              discard,          discard }
};

static int pico_igmp_process_event(struct igmp_packet_params *params)
{
  struct pico_tree_node *index;
  uint8_t ret = 0;
  struct mgroup_info *info = pico_igmp_find_mgroup(&(params->mcast_addr));

  igmp_dbg("IGMP: process event on group address %08X\n", params->mcast_addr.addr);
  if (NULL == info) {
    if (params->event == IGMP_EVENT_QUERY_RECV) { /* general query (mcast_addr field is zero) */
      pico_tree_foreach(index,&KEYTable) {
        info = index->keyValue;
        params->src_interface.addr = info->src_interface.addr;
        params->mcast_addr.addr = info->mgroup_addr.addr;
        igmp_dbg("IGMP: for each mcast_addr = %08X | state = %u\n", params->mcast_addr.addr, info->membership_state);
        ret |= host_membership_diagram_table[info->membership_state][params->event](params);
      }
    } else { /* first time this group enters the state diagram */
      igmp_dbg("IGMP: state = Non-Member\n");
      ret |= host_membership_diagram_table[IGMP_STATE_NON_MEMBER][params->event](params);
    }
  } else {
    igmp_dbg("IGMP: state = %u (0: non-member - 1: delaying member - 2: idle member)\n", info->membership_state); 
    ret |= host_membership_diagram_table[info->membership_state][params->event](params);
  }

  if( 0 == ret) {
    return 0;
  } else {
    igmp_dbg("IGMP ERROR: pico_igmp_process_event failed!\n");
    return -1;
  }
}

