/*******************************************************************************
 *
 * $Id$
 *
 * Author: Jukka Julku, VTT Technical Research Centre of Finland
 * -------
 *
 * Date:   10.1.2008
 * -----
 *
 * Description:
 * ------------
 *
 * Output functions for S-NET network interface.
 *
 *******************************************************************************/

#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

#include "memfun.h"
#include "output.h"
#include "snetentities.h"
#include "label.h"
#include "interface.h"
#include "stream_layer.h"

/* Thread to do the output */
static pthread_t thread; 

typedef struct { 
  FILE *file;
  snetin_label_t *labels;
  snetin_interface_t *interfaces;
  snet_tl_stream_t *buffer;
} handle_t;


/* This function prints records to stdout */
static void printRec(snet_record_t *rec, handle_t *hnd)
{
  int k = 0;
  int i = 0;
  char *label = NULL;
  char *interface = NULL;
  snet_record_mode_t mode;

  /* Change this to redirect the output! */

  if( rec != NULL) {

    fprintf(hnd->file, "<?xml version=\"1.0\" ?>");

    switch( SNetRecGetDescriptor( rec)) {
    case REC_data:

      mode = SNetRecGetDataMode(rec);
      
      if(mode == MODE_textual) {
	fprintf(hnd->file, "<record xmlns=\"snet-home.org\" type=\"data\" mode=\"textual\" >");
      }else {
	fprintf(hnd->file, "<record xmlns=\"snet-home.org\" type=\"data\" mode=\"binary\" >");
      }

      /* Fields */
      for( k=0; k<SNetRecGetNumFields( rec); k++) {
	i = SNetRecGetFieldNames( rec)[k];
	
	int id = SNetRecGetInterfaceId(rec);
	
	if((label = SNetInIdToLabel(hnd->labels, i)) != NULL){
	  if((interface = SNetInIdToInterface(hnd->interfaces, id)) != NULL) {
	    fprintf(hnd->file, "<field label=\"%s\" interface=\"%s\">", label, 
		    interface);
	    
	    if(mode == MODE_textual) { 
	      SNetGetSerializationFun(id)(hnd->file, SNetRecGetField(rec, i));
	    }else {
	      SNetGetEncodingFun(id)(hnd->file, SNetRecGetField(rec, i));
	    }
	    
	    fprintf(hnd->file, "</field>");
	    SNetMemFree(interface);
	  }

	  SNetMemFree(label);
	}else{
	  /* Error: unknown label! */
	}
	
      }
      
       /* Tags */
      for( k=0; k<SNetRecGetNumTags( rec); k++) {
	i = SNetRecGetTagNames( rec)[k];
	
	if((label = SNetInIdToLabel(hnd->labels, i)) != NULL){
	  fprintf(hnd->file, "<tag label=\"%s\">%d</tag>", label, SNetRecGetTag(rec, i));	   
	}else{
	  /* Error: unknown label! */
	}
	
	SNetMemFree(label);
      }
      
      /* BTags */
      for( k=0; k<SNetRecGetNumBTags( rec); k++) {
	i = SNetRecGetBTagNames( rec)[k];
	
	if((label = SNetInIdToLabel(hnd->labels, i)) != NULL){
	  fprintf(hnd->file, "<btag label=\"%s\">%d</btag>", label, SNetRecGetBTag(rec, i)); 
	}else{
	  /* Error: unknown label! */
	}
	
	SNetMemFree(label);
      }
      fprintf(hnd->file, "</record>");
      break;
    case REC_sync: /* TODO: What additional data is needed? */
      fprintf(hnd->file, "<record type=\"sync\" />");
    case REC_collect: /* TODO: What additional data is needed? */
      fprintf(hnd->file, "<record type=\"collect\" />");
    case REC_sort_begin: /* TODO: What additional data is needed? */
      fprintf(hnd->file, "<record type=\"sort_begin\" />");
    case REC_sort_end: /* TODO: What additional data is needed? */
      fprintf(hnd->file, "<record type=\"sort_end\" />");
    case REC_terminate:
      fprintf(hnd->file, "<record type=\"terminate\" />");
      break;
    default:
      break;
    }
  }

  fflush(hnd->file);
}

/* This is output function for the output thread */
static void *doOutput(void* data)
{
  handle_t *hnd = (handle_t *)data;

  snet_record_t *rec = NULL;
  if(hnd->buffer != NULL){
    while(1){
      rec = SNetTlRead(hnd->buffer);
      if(rec != NULL) {
      	printRec(rec, hnd);
	if(SNetRecGetDescriptor(rec) == REC_terminate){
	  SNetRecDestroy(rec);
	  break;
	}
	SNetRecDestroy(rec);
      }
    }
  }

  fflush(hnd->file);
  fprintf(hnd->file, "\n");
  SNetMemFree(hnd);

  return NULL;
}

int SNetInOutputInit(FILE *file,
		     snetin_label_t *labels, 
		     snetin_interface_t *interfaces, 
		     snet_tl_stream_t *in_buf)
{
  handle_t *hnd = SNetMemAlloc(sizeof(handle_t));

  hnd->file = file;
  hnd->labels = labels;
  hnd->interfaces = interfaces;
  hnd->buffer = in_buf;

  if(in_buf == NULL || pthread_create(&thread, NULL, (void *)doOutput, (void *)hnd) == 0){
    return 0;
  }
  
  /* error */
  return 1;
}

int SNetInOutputDestroy()
{

  if(pthread_join(thread, NULL) == 0){
    return 0;
  }
  /* Error */
  return 1;
}
