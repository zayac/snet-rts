#include <assert.h>

#include "snetentities.h"
#include "record_p.h"
#include "time.h"
#include "debug.h"
#include "memfun.h"

#include "lpelif.h"
#include "lpel.h"

#include "handle_p.h"

#ifdef DISTRIBUTED_SNET
#include "routing.h"
#endif

#ifdef SNET_DEBUG_COUNTERS
#include "debugtime.h"
#include "debugcounters.h"
#endif /* SNET_DEBUG_COUNTERS  */

//#define BOX_DEBUG

/* ------------------------------------------------------------------------- */
/*  SNetBox                                                                  */
/* ------------------------------------------------------------------------- */

typedef struct {
  lpel_stream_t *input, *output;
  void (*boxfun)( snet_handle_t*);
  snet_box_sign_t *out_signs;
  const char *boxname;
} box_arg_t;

/**
 * Box task
 */
static void BoxTask(lpel_task_t *self, void *arg)
{

#ifdef DBG_RT_TRACE_BOX_TIMINGS
  static struct timeval tv_in;
  static struct timeval tv_out;
#endif

#ifdef SNET_DEBUG_COUNTERS 
  snet_time_t time_in;
  snet_time_t time_out;
  long mseconds;
#endif /* SNET_DEBUG_COUNTERS */

  box_arg_t *barg = (box_arg_t *)arg;
  snet_record_t *rec;
  lpel_stream_desc_t *instream, *outstream;
  bool terminate = false;

  /* storage for the handle is within the box task */
  snet_handle_t hnd;

  instream = LpelStreamOpen(self, barg->input, 'r');
  outstream =  LpelStreamOpen(self, barg->output, 'w');

  /* set out descriptor */
  hnd.out_sd = outstream;
  /* set out signs */
  hnd.sign = barg->out_signs;
  /* mapping */
  hnd.mapping = NULL;

  /* MAIN LOOP */
  while( !terminate) {
    /* read from input stream */
    rec = LpelStreamRead( instream);

    switch( SNetRecGetDescriptor(rec)) {
      case REC_trigger_initialiser:
      case REC_data:
        hnd.rec = rec;

#ifdef DBG_RT_TRACE_BOX_TIMINGS
        gettimeofday( &tv_in, NULL);
        SNetUtilDebugNotice("[DBG::RT::TimeTrace SnetBox Calls %p at %lf\n",
                        &hnd, tv_in.tv_sec + tv_in.tv_usec / 1000000.0);
#endif
#ifdef SNET_DEBUG_COUNTERS 
	SNetDebugTimeGetTime(&time_in);
#endif /* SNET_DEBUG_COUNTERS */

        (*barg->boxfun)( &hnd);

#ifdef DBG_RT_TRACE_BOX_TIMINGS
        gettimeofday( &tv_out, NULL);
        SNetUtilDebugNotice("[DBG::RT::TimeTrace] SnetBox resumes from %p after "
                    "%lf\n\n", &hnd, (tv_out.tv_sec - tv_in.tv_sec) +
                        (tv_out.tv_usec - tv_in.tv_usec) / 1000000.0);
#endif
#ifdef SNET_DEBUG_COUNTERS
	SNetDebugTimeGetTime(&time_out);

	mseconds = SNetDebugTimeDifferenceInMilliseconds(&time_in, &time_out);

	SNetDebugCountersIncreaseCounter(mseconds, SNET_COUNTER_TIME_BOX);
#endif /* SNET_DEBUG_COUNTERS */
        SNetRecDestroy( rec);

        /* restrict to one data record per execution */
        //LpelTaskYield( self);
        break;

      case REC_sync:
        {
          lpel_stream_t *newstream = (lpel_stream_t*) SNetRecGetStream(rec);
          LpelStreamReplace( instream, newstream);
          SNetRecDestroy( rec);
        }
        break;

      case REC_collect:
        assert(0);
        /* if ignore, destroy at least ...*/
        SNetRecDestroy( rec);
        break;

      case REC_sort_end:
        /* forward the sort record */
        LpelStreamWrite( outstream, rec);
        break;

      case REC_terminate:
        LpelStreamWrite( outstream, rec);
        terminate = true;
        break;

      default:
        assert(0);
        /* if ignore, destroy at least ...*/
        SNetRecDestroy( rec);
    }
  } /* MAIN LOOP END */

  LpelStreamClose( instream, true);
  LpelStreamClose( outstream, false);

  SNetHndDestroy( &hnd);
  
  /* destroy box arg */
  SNetTencBoxSignDestroy( barg->out_signs);
  SNetMemFree( barg);
}


/**
 * Box creation function
 */
snet_stream_t *SNetBox( snet_stream_t *input,
    snet_info_t *info, 
#ifdef DISTRIBUTED_SNET
    int location,
#endif /* DISTRIBUTED_SNET */ 
    const char *boxname,
    snet_box_fun_t boxfun,
    snet_box_sign_t *out_signs) 
{
  snet_stream_t *output;
  box_arg_t *barg;

#ifdef DISTRIBUTED_SNET
  input = SNetRoutingContextUpdate(SNetInfoGetRoutingContext(info), input, location);
  if(location == SNetIDServiceGetNodeID()) {
#ifdef DISTRIBUTED_DEBUG
    SNetUtilDebugNotice("Box \"%s\" created", boxname);
#endif /* DISTRIBUTED_DEBUG */
#endif /* DISTRIBUTED_SNET */

    output = (snet_stream_t*) LpelStreamCreate();
    
    barg = (box_arg_t *) SNetMemAlloc( sizeof( box_arg_t));
    barg->input  = (lpel_stream_t*) input;
    barg->output = (lpel_stream_t*) output;
    barg->boxfun = boxfun;
    barg->out_signs = out_signs;
    barg->boxname = boxname;

    SNetLpelIfSpawnEntity( BoxTask, (void*)barg, ENTITY_box, (char*) boxname);
    
#ifdef DISTRIBUTED_SNET
  } else {
    SNetTencBoxSignDestroy(out_signs);
    output = input;
  }
#endif /* DISTRIBUTED_SNET */
  return( output);
}

