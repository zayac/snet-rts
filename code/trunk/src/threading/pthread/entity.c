#if defined(__linux__) && defined(HAVE_PTHREAD_SETAFFINITY_NP)
#define USE_CORE_AFFINITY 1
#endif

#undef USE_CORE_AFFINITY

#ifdef USE_CORE_AFFINITY
#define _GNU_SOURCE
#endif


#include <stdlib.h>
#include <stdio.h>

#include <pthread.h>
#include <assert.h>

#include "threading.h"

#include "entity.h"
#include "monitorer.h"

#include "distribution.h"

static unsigned int entity_count = 0;
static pthread_mutex_t entity_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  entity_cond = PTHREAD_COND_INITIALIZER;
static pthread_key_t entity_self_key;

/* prototype for pthread thread function */
static void *SNetEntityThread(void *arg);

static size_t SNetEntityStackSize(snet_entity_type_t type);

static void SNetEntityExit(snet_entity_t *self);


#ifdef USE_CORE_AFFINITY
typedef enum {
  DEFAULT,
  MASKMOD2,
  STRICTLYFIRST,
  ALLBUTFIRST
} affinity_type_t;

static int SNetSetThreadAffinity( pthread_t *pt, affinity_type_t at);
#endif



int SNetThreadingInit(int argc, char **argv)
{
  char fname[32];
  /* initialize the entity counter to 0 */
  entity_count = 0;
  pthread_key_create(&entity_self_key, NULL);

  /* initialize the monitorer */

  snprintf(fname, 31, "mon_n%02u_info.log", SNetDistribGetNodeId());
  SNetThreadingMonitoringInit(fname);

  return 0;
}



void SNetThreadingStop(void)
{
  /* NOP */
}



int SNetThreadingProcess(void)
{
  /* Wait for the entities */
  pthread_mutex_lock( &entity_lock );
  while (entity_count > 0) {
    pthread_cond_wait( &entity_cond, &entity_lock );
  }
  pthread_mutex_unlock( &entity_lock );

  return 0;
}


int SNetThreadingCleanup(void)
{
  pthread_key_delete(entity_self_key);
  SNetThreadingMonitoringCleanup();
  return 0;
}


int SNetEntitySpawn(
  snet_entity_type_t type,
  snet_locvec_t *locvec,
  int location,
  const char *name,
  snet_entityfunc_t func,
  void *arg
  )
{
  int res;
  pthread_t p;
  pthread_attr_t attr;
  size_t stacksize;

  /* create snet_entity_t */
  snet_entity_t *self = malloc(sizeof(snet_entity_t));
  self->func = func;
  self->inarg = arg;
  pthread_mutex_init( &self->lock, NULL );
  pthread_cond_init( &self->pollcond, NULL );
  self->wakeup_sd = NULL;


  /* increment entity counter */
  pthread_mutex_lock( &entity_lock );
  entity_count += 1;
  pthread_mutex_unlock( &entity_lock );


  /* create pthread: */

  (void) pthread_attr_init( &attr);

  /* stacksize */
  stacksize = SNetEntityStackSize(type);

  if (stacksize > 0) {
    res = pthread_attr_setstacksize(&attr, stacksize);
    if (res != 0) {
      //error(1, res, "Cannot set stack size!");
      return 1;
    }
  }

  /* all threads are detached */
  res = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (res != 0) {
    //error(1, res, "Cannot set detached!");
    return 1;
  }

  /* spawn off thread */
  res = pthread_create( &p, &attr, SNetEntityThread, self);
  if (res != 0) {
    //error(1, res, "Cannot create pthread!");
    return 1;
  }
  pthread_attr_destroy( &attr);


  /* core affinity */
#ifdef USE_CORE_AFFINITY
  if( type == ENTITY_other) {
    SNetSetThreadAffinity( &p, STRICTLYFIRST);
  } else {
    SNetSetThreadAffinity( &p, ALLBUTFIRST);
  }
#endif /* USE_CORE_AFFINITY */


  return 0;
}


void SNetThreadingEventSignal(snet_threading_event_t evt)
{
  //FIXME replace by sensible data
  SNetThreadingMonitoringAppend( (struct snet_moninfo_t *) ((void*)0x1000+evt) );
}

void SNetEntityYield(void)
{
#ifdef _GNU_SOURCE
  (void) pthread_yield();
#endif
}


snet_entity_t *SNetEntitySelf(void)
{
  return (snet_entity_t *) pthread_getspecific(entity_self_key);
}


/******************************************************************************
 * Private functions
 *****************************************************************************/

static void *SNetEntityThread(void *arg)
{
  snet_entity_t *ent = (snet_entity_t *)arg;

  pthread_setspecific(entity_self_key, ent);

  ent->func( ent->inarg );
  /* call entity function */
  SNetEntityExit(ent);

  /* following line is not reached, just for compiler happiness */
  return NULL;
}

/**
 * Let the current entity exit
 */
static void SNetEntityExit(snet_entity_t *self)
{
  /* cleanup */
  pthread_mutex_destroy( &self->lock );
  pthread_cond_destroy( &self->pollcond );
  free(self);


  /* decrement and signal entity counter */
  pthread_mutex_lock( &entity_lock );
  entity_count -= 1;
  if (entity_count == 0) {
    pthread_cond_signal( &entity_cond );
  }
  pthread_mutex_unlock( &entity_lock );


  (void) pthread_exit(NULL);
}

static size_t SNetEntityStackSize(snet_entity_type_t type)
{
  size_t stack_size;

  switch(type) {
    case ENTITY_parallel:
    case ENTITY_star:
    case ENTITY_split:
    case ENTITY_fbcoll:
    case ENTITY_fbdisp:
    case ENTITY_fbbuf:
    case ENTITY_sync:
    case ENTITY_filter:
    case ENTITY_collect:
      stack_size = 256*1024; /* HIGHLY EXPERIMENTAL! */
      break;
    case ENTITY_box:
    case ENTITY_other:
      /* return 0 (caller will use default stack size) */
      stack_size = 0;
      break;
    default:
      /* we do not want an unhandled case here */
      assert(0);
  }

 return( stack_size);
}

#ifdef USE_CORE_AFFINITY
static int SNetSetThreadAffinity( pthread_t *pt, affinity_type_t at)
{
  int i, res, numcpus;
  cpu_set_t cpuset;

  res = pthread_getaffinity_np( *pt, sizeof(cpu_set_t), &cpuset);
  if (res != 0) {
    return 1;
  }

  numcpus = CPU_COUNT(&cpuset);

  switch(at) {
    case MASKMOD2:
      CPU_ZERO(&cpuset);
      for( i=0; i<numcpus; i+=2) {
        CPU_SET( i, &cpuset);
      }
      break;
    case STRICTLYFIRST:
      CPU_ZERO(&cpuset);
      CPU_SET(0, &cpuset);
      break;
    case ALLBUTFIRST:
      CPU_CLR(0, &cpuset);
      break;
    default:
      break;
  }

  /* set the affinity mask */
  res = pthread_setaffinity_np( *pt, sizeof(cpu_set_t), &cpuset);
  if( res != 0) {
    return 1;
  }

  return 0;
}
#endif

