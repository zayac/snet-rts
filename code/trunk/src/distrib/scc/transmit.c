#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "SCC_API.h"
#include "distribcommon.h"
#include "memfun.h"
#include "interface_functions.h"
#include "record.h"
#include "reference.h"
#include "scc.h"
#include "sccmalloc.h"

static t_vcharp sendMPB;
static t_vcharp recvMPB;

static void write_pid(void)
{
    FILE *file = fopen("/sys/module/async_scc/parameters/pid", "w");
    if (file == NULL) {
        perror("Could not open module parameter");
        exit(EXIT_FAILURE);
    }
    fprintf(file, "%d", getpid());
    fclose(file);
}

static void PackInt(void *buf, int count, int *src)
{ cpy_mem_to_mpb(buf, src, count * sizeof(int)); }

static void UnpackInt(void *buf, int count, int *dst)
{ cpy_mpb_to_mem(buf, dst, count * sizeof(int)); }

static void PackByte(void *buf, int count, char *src)
{ cpy_mem_to_mpb(buf, src, count); }

static void UnpackByte(void *buf, int count, char *dst)
{ cpy_mpb_to_mem(buf, dst, count); }

static void SCCPackInt(int count, int *src)
{ PackInt((void*) sendMPB, count, src); }

static void SCCUnpackInt(int count, int *dst)
{ UnpackInt((void*) recvMPB, count, dst); }

void SCCPackRef(int count, snet_ref_t **src)
{
  int i;
  for (i = 0; i < count; i++) {
    SNetRefSerialise(src[i], (void*) sendMPB, &PackInt, &PackByte);
    SNetRefOutgoing(src[i]);
  }
}

void SCCUnpackRef(int count, snet_ref_t **dst)
{
  int i;
  for (i = 0; i < count; i++) {
    dst[i] = SNetRefDeserialise((void*) recvMPB, &UnpackInt, &UnpackByte);
    SNetRefIncoming(dst[i]);
  }
}

snet_msg_t SNetDistribRecvMsg(void)
{
  int sig;
  lut_addr_t addr;
  snet_msg_t result;
  static sigset_t sigmask;
  static bool handling = true, set = false;

  if (!set) {
    set = true;
    write_pid();
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGUSR1);
    sigaddset(&sigmask, SIGUSR2);
  }

  recvMPB = mpbs[node_location];

start:
  if (!handling) {
    sigwait(&sigmask, &sig);
    if (sig == SIGUSR2) {
      result.type = snet_update;
      return result;
    }
  }

  lock(node_location);

  flush();
  if (START(recvMPB) == END(recvMPB)) {
    handling = false;
    HANDLING(recvMPB) = 0;
    FOOL_WRITE_COMBINE;
    unlock(node_location);
    goto start;
  } else {
    handling = true;
    HANDLING(recvMPB) = 1;
    FOOL_WRITE_COMBINE;
  }

  cpy_mpb_to_mem(recvMPB, &result.type, sizeof(snet_comm_type_t));
  switch (result.type) {
    case snet_rec:
      result.rec = SNetRecDeserialise(&SCCUnpackInt, &SCCUnpackRef);
    case snet_block:
    case snet_unblock:
      cpy_mpb_to_mem(recvMPB, &result.dest, sizeof(snet_dest_t));
      break;
    case snet_ref_set:
      result.ref = SNetRefDeserialise((void*) recvMPB, &UnpackInt, &UnpackByte);
      cpy_mpb_to_mem(recvMPB, &addr, sizeof(lut_addr_t));
      result.data = SNetInterfaceGet(SNetRefInterface(result.ref))->unpackfun((void*) &addr);
      break;
    case snet_ref_fetch:
      result.ref = SNetRefDeserialise((void*) recvMPB, &UnpackInt, &UnpackByte);
      result.data = SNetMemAlloc(sizeof(lut_addr_t));
      cpy_mpb_to_mem(recvMPB, result.data, sizeof(lut_addr_t));
      break;
    case snet_ref_update:
      result.ref = SNetRefDeserialise((void*) recvMPB, &UnpackInt, &UnpackByte);
      cpy_mpb_to_mem(recvMPB, &result.val, sizeof(int));
      break;
    default:
      break;
  }

  unlock(node_location);
  return result;
}

void SNetDistribSendRecord(snet_dest_t dest, snet_record_t *rec)
{
  int node = dest.node;
  snet_comm_type_t type = snet_rec;
  sendMPB = mpbs[node];

  start_write_node(node);
  cpy_mem_to_mpb(sendMPB, &type, sizeof(snet_comm_type_t));

  SNetRecSerialise(rec, &SCCPackInt, &SCCPackRef);
  dest.node = node_location;
  cpy_mem_to_mpb(sendMPB, &dest, sizeof(snet_dest_t));
  dest.node = node;
  stop_write_node(node);
}

extern size_t SNetRefAllocSize(snet_ref_t *ref);
void SNetDistribFetchRef(snet_ref_t *ref)
{
  int node = SNetRefNode(ref);
  lut_addr_t addr = SCCPtr2Addr(SCCMalloc(SNetRefAllocSize(ref)));
  snet_comm_type_t type = snet_ref_fetch;

  start_write_node(node);
  cpy_mem_to_mpb(mpbs[node], &type, sizeof(snet_comm_type_t));
  SNetRefSerialise(ref, (void*) mpbs[node], &PackInt, &PackByte);
  cpy_mem_to_mpb(mpbs[node], &addr, sizeof(lut_addr_t));
  stop_write_node(node);
}

void SNetDistribUpdateRef(snet_ref_t *ref, int count)
{
  int node = SNetRefNode(ref);
  snet_comm_type_t type = snet_ref_update;

  start_write_node(node);
  cpy_mem_to_mpb(mpbs[node], &type, sizeof(snet_comm_type_t));
  SNetRefSerialise(ref, (void*) mpbs[node], &PackInt, &PackByte);
  cpy_mem_to_mpb(mpbs[node], &count, sizeof(int));
  stop_write_node(node);
}

void SNetDistribUpdateBlocked(void)
{
  snet_comm_type_t type = snet_update;
  start_write_node(node_location);
  cpy_mem_to_mpb(mpbs[node_location], &type, sizeof(int));
  stop_write_node(node_location);
}

void SNetDistribUnblockDest(snet_dest_t dest)
{
  snet_comm_type_t type = snet_unblock;
  start_write_node(dest.node);
  cpy_mem_to_mpb(mpbs[dest.node], &type, sizeof(snet_comm_type_t));
  cpy_mem_to_mpb(mpbs[dest.node], &dest, sizeof(snet_dest_t));
  stop_write_node(dest.node);
}

void SNetDistribBlockDest(snet_dest_t dest)
{
  snet_comm_type_t type = snet_block;
  start_write_node(dest.node);
  cpy_mem_to_mpb(mpbs[dest.node], &type, sizeof(snet_comm_type_t));
  cpy_mem_to_mpb(mpbs[dest.node], &dest, sizeof(snet_dest_t));
  stop_write_node(dest.node);
}

void SNetDistribSendData(snet_ref_t *ref, void *data, void *dest)
{
  lut_addr_t *addr = dest;
  snet_comm_type_t type = snet_ref_set;

  start_write_node(addr->node);
  cpy_mem_to_mpb(mpbs[addr->node], &type, sizeof(snet_comm_type_t));
  SNetRefSerialise(ref, (void*) mpbs[addr->node], &PackInt, &PackByte);
  cpy_mem_to_mpb(mpbs[addr->node], addr, sizeof(lut_addr_t));
  SNetInterfaceGet(SNetRefInterface(ref))->packfun(data, addr);
  stop_write_node(addr->node);
  SNetMemFree(dest);
}
