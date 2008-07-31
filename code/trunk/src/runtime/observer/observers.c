/*******************************************************************************
 *
 * $Id$
 *
 * Author: Jukka Julku, VTT Technical Research Centre of Finland
 * -------
 *
 * Date:   4.4.2008
 * -----
 *
 * Description:
 * ------------
 *
 * SNet observer implementation
 *
 *******************************************************************************/

#include <stdlib.h>
#include <pthread.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <regex.h>

#include "bool.h"
#include "constants.h"
#include "record.h"
#include "typeencode.h"
#include "buffer.h"
#include "memfun.h"
#include "bool.h"
#include "interface_functions.h"
#include "observers.h"

/* Size of buffer used to transfer unhanled data from received message to next message. */
#define MSG_BUF_SIZE 256

/* Size of notification buffer. */
#define NOTIFICATION_BUFFER_SIZE 4

/* Size of the buffer used to receive data. */
#define BUF_SIZE 256

/* Observer type. */
typedef enum {OBSfile, OBSsocket} obstype_t;


/* A handle for observer data. */
typedef struct obs_handle {
  obstype_t obstype;     // type of the observer
  void *desc;            // Observer desctiptor, depends on the type of the observer (obs_socket_t / obs_file_T)
  bool isInteractive;    // Is this observer interactive
  snet_buffer_t *inbuf;  // Buffer for incoming records
  snet_buffer_t *outbuf; // Buffer for outgoing records
  int id;                // Instance specific ID of this observer (NOTICE: this will change!)
  const char *code;      // Code attribute
  const char *position;  // Name of the net/box to which the observer is attached to
  char type;             // Type of the observer (before/after)
  char data_level;       // Data level used
}obs_handle_t;


/* Wait queue for observers waiting for reply messages */
typedef struct obs_wait_queue {
  obs_handle_t *hnd;           // Observer
  pthread_cond_t cond;         // Wait condition
  struct obs_wait_queue *next;
} obs_wait_queue_t;


/* Data for socket */
typedef struct obs_socket{
  int fdesc;                    // Socket file descriptor
  FILE *file;                   // File pointer to the file descriptor
  int users;                    // Number of users currently registered for this socket.
  struct hostent *host;         // Socket data
  struct sockaddr_in addr;      
  obs_wait_queue_t *wait_queue; // Queue of currently waiting observers.
  char buffer[MSG_BUF_SIZE];    // Buffer for buffering leftovers of last message                       
  struct obs_socket *next; 
}obs_socket_t; 


/* Data for file */
typedef struct obs_file {
  FILE *file;             // File
  int users;              // Number of users currently registered for this file.
  char *filename;         // Name of the file
  struct obs_file *next;
} obs_file_t;



/* Thread for dispatcher. This is needed for pthread_join later. */
static pthread_t *dispatcher_thread;

/* Mutex to guard connections list */
static pthread_mutex_t connection_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Registered sockets / files */
static obs_socket_t *open_sockets = NULL;
static obs_file_t *open_files = NULL;
static int user_count = 0;


/* This value indicates if the dispatcher should terminate */
static volatile bool mustTerminate = false;

/* This pipe is used to send messages to dispatcher thread. (Wake it up from select). */
static int notification_pipe[2];

/* Pattern of reply message for regex. */
static const char pattern[] = "^(<[\?]xml[ \n\t]+version=\"1[.]0\"[ \n\t]*[\?]>)?<observer[ \n\t]+(xmlns=\"snet-home[.]org\"[ \n\t]+)?oid=\"([0-9]+)\"([ \n\t]+xmlns=\"snet-home[.]org\"[ \n\t]+)?[ \n\t]*((/>)|(>[ \n\t]*</observer[ \n\t]*>))[ \n\t]*";

/* Compiled regex */
static regex_t *preg = NULL;

/* These are used to provide instance specific IDs for observers */
static int id_pool = 0;

static snetin_label_t *labels = NULL;
static snetin_interface_t *interfaces = NULL;



/** <!--********************************************************************-->
 *
 * @fn obs_socket_t *ObserverInitSocket(const char *addr, int port)
 *
 *   @brief  Open new socket for an observer.
 * 
 *           The socket is automatically added to the list of sockets
 *           and the dispatcher is notified of new socket. 
 *
 *   @param addr  The address of the listener
 *   @param port  The port used by the listener
 *   @return      The new socket, or NULL if the operation failed.
 *
 ******************************************************************************/

static obs_socket_t *ObserverInitSocket(const char *addr, int port)
{
  obs_socket_t *new = SNetMemAlloc(sizeof(obs_socket_t));
  int i = 0;
  
  if(new == NULL){
    return NULL;
  }

  new->wait_queue = NULL;
  new->next = NULL;

  /* Open connection */
  if(inet_aton(addr, &new->addr.sin_addr)) {
    SNetMemFree(new);
    return NULL;
  }

  new->host = gethostbyname(addr);

  if (!new->host) {
    SNetMemFree(new);
    return NULL;
  }

  new->addr.sin_addr = *(struct in_addr*)new->host->h_addr;
  new->fdesc = socket(PF_INET, SOCK_STREAM, 0);

  if(new->fdesc == -1){
    SNetMemFree(new);
    return NULL;
  }

  new->addr.sin_port = htons(port);
  new->addr.sin_family = AF_INET;
  
  if(connect(new->fdesc, (struct sockaddr*)&new->addr, sizeof(new->addr)) == -1) {
    SNetMemFree(new);
    return NULL;
  }

  new->file = fdopen(new->fdesc, "w");
  
  if(new->file == NULL) {
    SNetMemFree(new);
    return NULL;
  }

  /* Zero the message buffer. */
  for(i = 0; i < MSG_BUF_SIZE; i++){
    new->buffer[i] = '\0';
  }
  
  /* Wake up the dispatcher thread in select. A new file has to be added! */
  write(notification_pipe[1], "?", 1);

  new->next = open_sockets;
  open_sockets = new;

  return new;
}

/** <!--********************************************************************-->
 *
 * @fn void ObserverWait(obs_handle_t *self)
 *
 *   @brief  Put observer to wait for a reply message.
 * 
 *           Observer blocks until it is released by the dispatcher.
 * 
 *   @param self  Handle of the observer.
 *
 ******************************************************************************/

static void ObserverWait(obs_handle_t *self)
{
  obs_socket_t *socket;
  obs_wait_queue_t *temp;

  if(self == NULL || self->obstype != OBSsocket || self->desc == NULL) {
    return;
  }

  /* Search for unused wait queue places. */

  socket = (obs_socket_t *)self->desc;

  temp = socket->wait_queue;

  while(temp != NULL) {
    if(temp->hnd == NULL) {
      temp->hnd = self;

      pthread_cond_wait(&temp->cond, &connection_mutex);
      temp->hnd = NULL;

      return;
    }
    
    temp = temp->next;
  }
  
  /* No free wait queue structs. New one is created. */

  temp = SNetMemAlloc( sizeof(obs_wait_queue_t));

  if(temp == NULL){
    return;
  }

  pthread_cond_init(&temp->cond, NULL);
  temp->hnd = self;
  temp->next = socket->wait_queue;
  socket->wait_queue = temp;

  pthread_cond_wait(&temp->cond, &connection_mutex);
  temp->hnd = NULL;

  return;
}

/** <!--********************************************************************-->
 *
 * @fn int ObserverParseReplyMessage(char *buf, int *oid)
 *
 *   @brief  This function parses a reply message and returns ID of the
 *           corresponding observer.
 *
 *   @param buf   Buffer containing the message.
 *   @param oid   Variable to where the ID is stored.
 *   @return      Number of parsed characters.
 *
 ******************************************************************************/

static int ObserverParseReplyMessage(char *buf, int *oid)
{
  int ret = 0;
  regmatch_t pmatch[4];
  char id[32];
  int i = 0;
  *oid = -1;

  if(preg == NULL){
    return 0;
  }

  if((ret =  regexec(preg, buf, (size_t)4, pmatch, 0)) != 0){
    return 0;
  }

  /* Take oid. */
  for(i = pmatch[3].rm_so; i < pmatch[3].rm_eo; i++){
    id[i - pmatch[3].rm_so] = buf[i];
  }
  id[i - pmatch[3].rm_so] = '\0';

  *oid = atoi(id);

  return pmatch[0].rm_eo;
}

/** <!--********************************************************************-->
 *
 * @fn void *ObserverDispatch(void *arg) 
 *
 *   @brief  This is the main function for the dispatcher thread.
 *
 *           Dispatcher listens to all open sockets and parses possible reply 
 *           messages. Blocked interactive observers are released when 
 *           corresponding reply message is received.
 *
 *   @param arg   Possible data structures needed by the dispatcher
 *   @return      Always returns NULL
 *
 ******************************************************************************/

static void *ObserverDispatch(void *arg) 
{
  fd_set fdr_set;
  int ndfs = 0;
  int ret;
  obs_socket_t *temp = NULL;
  int len = 0;
  int i = 0;

  pthread_mutex_lock(&connection_mutex);

  /* Loop until the observer system is terminated */
  while(mustTerminate == false ||  user_count > 0){
    /* Form the read set. */
    ndfs = 0;
    temp = NULL;
    FD_ZERO(&fdr_set);
    
    /* Notification pipe is used to end select. */
    temp = open_sockets;
    FD_SET(notification_pipe[0], &fdr_set);
    ndfs = notification_pipe[0] + 1;

    while(temp != NULL){
      FD_SET(temp->fdesc, &fdr_set);
      ndfs = (temp->fdesc + 1 > ndfs) ? temp->fdesc + 1 : ndfs;
      temp = temp->next;
    }

    pthread_mutex_unlock(&connection_mutex);

    if(ndfs > 0){

      if((ret = select(ndfs, &fdr_set,  NULL, NULL, NULL)) == -1){

	/* Error. */
	pthread_mutex_lock(&connection_mutex);
      }
      else if(FD_ISSET(notification_pipe[0], &fdr_set)){
	char buffer[NOTIFICATION_BUFFER_SIZE];

	/* Something changed. What is written to the ontification pipe is meaningles. */

	read(notification_pipe[0], buffer, NOTIFICATION_BUFFER_SIZE);
	pthread_mutex_lock(&connection_mutex);
      }
      else{
	/* Incoming message(s). Go through all the changed files and parse messages */

	pthread_mutex_lock(&connection_mutex);
	temp = open_sockets;

	while(temp != NULL){
	  if(FD_ISSET(temp->fdesc, &fdr_set)){
	    char buf[BUF_SIZE];
	    int buflen;

	    for(i = 0; i < strlen(temp->buffer); i++){
	      buf[i] = temp->buffer[i];
	    }

	    buflen = strlen(temp->buffer);

	    len = recv(temp->fdesc, buf + buflen, BUF_SIZE - buflen, 0);

	    buf[buflen + len] = '\0';

	    if(len > 0){
	      int oid;
	      int offset = 0;
	      int ret = 0;

	      /* Parse messages. There might be multiple messages. */
	      do{
		ret = ObserverParseReplyMessage(buf + offset, &oid);
		offset += ret;

		/* Nothing was parsed. Buffer the message to be used 
		 * when the next messge arrives. */
		if(ret == 0 || oid == -1){
		  for(i = 0; i < strlen(buf);i++){
		    temp->buffer[i] = buf[offset + i];
		  }

		  for(; i < 128; i++){
		    temp->buffer[i] = '\0';
		  }
		  break;
		}

		/* Wake up sleeping observer */ 
		obs_wait_queue_t *queue = temp->wait_queue;
		while(queue != NULL){
		  if(queue->hnd != NULL && queue->hnd->id == oid) {
		    pthread_cond_signal(&queue->cond);
		    break;
		  }
		  queue = queue->next;
		}
	      }while(1);
	    }
	  }
	  temp = temp->next;
	}
      }
    }else{
      pthread_mutex_lock(&connection_mutex);
    }
  }
  pthread_mutex_unlock(&connection_mutex);

  return NULL;
}

/** <!--********************************************************************-->
 *
 * @fn int ObserverRegisterSocket(obs_handle_t *self,  const char *addr, int port)
 *
 *   @brief  Register an observer for socket output connection.
 *
 *   @param self  Observer handle of the observer to be registered.
 *   @param addr  The address of the listener
 *   @param port  The port used by the listener
 *   @return      0 if the operation succeed, -1 otherwise.
 *
 ******************************************************************************/

static int ObserverRegisterSocket(obs_handle_t *self,  const char *addr, int port)
{
  obs_socket_t *temp = open_sockets;
  struct hostent *host;

  pthread_mutex_lock(&connection_mutex);

  if(mustTerminate == true){
    pthread_mutex_unlock(&connection_mutex);
    return -1;
  }

  /* Test if the connection has already been opened */
  host = gethostbyname(addr);

  while(temp != NULL){
    if((*(struct in_addr*)host->h_addr).s_addr == temp->addr.sin_addr.s_addr
       && temp->addr.sin_port == htons(port)) {

      user_count++;
      temp->users++;

      self->obstype = OBSsocket;
      self->desc = (void *)temp;

      pthread_mutex_unlock(&connection_mutex);
      return 0;
    }
    temp = temp->next;
  }

  /* New connection */

  if((temp = ObserverInitSocket(addr, port)) == NULL) {
    pthread_mutex_unlock(&connection_mutex);
    return -1;
  }
  
  user_count++;
  temp->users = 1;

  self->obstype = OBSsocket;
  self->desc = (void *)temp;

  pthread_mutex_unlock(&connection_mutex);
  return 0;
}

/** <!--********************************************************************-->
 *
 * @fn int ObserverRegisterFile(obs_handle_t *self, const char *filename)
 *
 *   @brief  Register an observer for output file.
 *
 *   @param self      Observer handle of the observer to be registered.
 *   @param filename  The name of the file to be used.
 *   @return          0 if the operation succeed, -1 otherwise.
 *
 ******************************************************************************/

static int ObserverRegisterFile(obs_handle_t *self, const char *filename)
{
  obs_file_t *temp = open_files;
  FILE *file = NULL;
  
  pthread_mutex_lock(&connection_mutex);

  if(mustTerminate == true){
    pthread_mutex_unlock(&connection_mutex);
    return -1;
  }

  while(temp != NULL){
    if(strcmp(temp->filename, filename) == 0) {

      user_count++;
      temp->users++;

      self->obstype = OBSfile;
      self->desc = (void *)temp;

      pthread_mutex_unlock(&connection_mutex);
      return 0;
    }

    temp = temp->next;
  }

  /* The file is not open yet: */

  file = fopen(filename, "a");

  if(file == NULL) {
    pthread_mutex_unlock(&connection_mutex);
    return -1;
  }

  temp = SNetMemAlloc(sizeof(obs_file_t));

  if(temp == NULL) {
    fclose(file);
    pthread_mutex_unlock(&connection_mutex);
    return -1;
  }

  temp->file = file;   

  user_count++;
  temp->users = 1;


  temp->filename = SNetMemAlloc(sizeof(char) * (strlen(filename) + 1));

  if(temp->filename == NULL) {
    fclose(file);
    pthread_mutex_unlock(&connection_mutex);
    return -1;
  }

  temp->filename = strcpy(temp->filename, filename);

  temp->next = open_files;
  open_files = temp;  

  self->obstype = OBSfile; 
  self->desc = (void *)temp;

  pthread_mutex_unlock(&connection_mutex);
  return 0;
}

/** <!--********************************************************************-->
 *
 * @fn void ObserverRemove(obs_handle_t *self)
 *
 *   @brief  Unregister observer from the observer system.
 *
 *   @param self      Observer handle of the observer to be unregistered.
 *
 ******************************************************************************/

static void ObserverRemove(obs_handle_t *self)
{
  obs_file_t *file;
  obs_socket_t *socket;
/*
  obs_file_t *temp_file;
  obs_socket_t *temp_socket;
  obs_wait_queue_t *queue;
*/

  if(self == NULL) {
    return;
  }

  pthread_mutex_lock(&connection_mutex);
  
  if(self->obstype == OBSfile) {
    if(self->desc != NULL) {
      file = (obs_file_t *)self->desc;
      
      file->users--;

      /*
      if(file->users == 0) {
	fclose(file->file);

	if(file == open_files) {
	  open_files = file->next;
	} else {
	  temp_file = open_files;

	  while(temp_file->next != NULL) {
	    if(temp_file->next == file) {
	      temp_file->next = file->next;
	      break;
	    }

	    temp_file = temp_file->next;
	  }
	}
	
	SNetMemFree(file->filename);
	SNetMemFree(file);
	
      }
      */

      user_count--;

    }
  } else if(self->obstype == OBSsocket) {
    if(self->desc != NULL) {
      socket = (obs_socket_t *)self->desc;

      socket->users--;
      /*
      if(socket->users == 0) {
	fclose(socket->file);

	if(socket == open_sockets) {
	  open_sockets = socket->next;
	} else {
	  temp_socket = open_sockets;
	  
	  while(temp_socket->next != NULL) {
	    if(temp_socket->next == socket) {
	      temp_socket->next = socket->next;
	      break;
	    }

	    temp_socket = temp_socket->next;
	  }
	}

	while(socket->wait_queue != NULL) {
	  queue = socket->wait_queue;

	  pthread_cond_destroy(&socket->wait_queue->cond);

	  SNetMemFree(socket->wait_queue);

	  socket->wait_queue = queue;
	}
	
	SNetMemFree(socket);
      }
      */

      user_count--;

    }
  }

  pthread_mutex_unlock(&connection_mutex);

  return;
}

/** <!--********************************************************************-->
 *
 * @fn int ObserverPrintRecordToFile(FILE *file, obs_handle_t *hnd, snet_record_t *rec)
 *
 *   @brief  Write record into the given output stream.

 *
 *   @param file  File pointer to where the record is printed.
 *   @param hnd   Handle of the observer.
 *   @param rec   Record to be printed.
 *   @return      0 in case of success, -1 in case of failure.
 *
 ******************************************************************************/

static int ObserverPrintRecordToFile(FILE *file, obs_handle_t *hnd, snet_record_t *rec)
{
  int k, i;
  char *label = NULL;
  char *interface = NULL;
  snet_record_mode_t mode;
  int (*fun)(FILE *,void *);
  
  fprintf(file,"<?xml version=\"1.0\"?>");
  fprintf(file,"<observer xmlns=\"snet-home.org\" oid=\"%d\" position=\"%s\" type=\"", hnd->id, hnd->position);

  if(hnd->type == SNET_OBSERVERS_TYPE_BEFORE) {
    fprintf(file,"before\" ");
  } else if(hnd->type == SNET_OBSERVERS_TYPE_AFTER){
    fprintf(file,"after\" ");
  }

  if(hnd->code != NULL){
    fprintf(file,"code=\"%s\" ", hnd->code);
  }

  fprintf(file,">");
  
  switch(SNetRecGetDescriptor( rec)) {

  case REC_data:
    mode = SNetRecGetDataMode(rec);
    
    if(mode == MODE_textual) {
      fprintf(file, "<record type=\"data\" mode=\"textual\" >");
    }else {
      fprintf(file, "<record type=\"data\" mode=\"binary\" >");
    }

    /* fields */
    for(k=0; k<SNetRecGetNumFields( rec); k++) {    
      int id = SNetRecGetInterfaceId(rec);  
   
      if(mode == MODE_textual) {
	fun = SNetGetSerializationFun(id);
      }else {
	fun = SNetGetEncodingFun(id);
      }

      i = SNetRecGetFieldNames( rec)[k];
      if((label = SNetInIdToLabel(labels, i)) != NULL){
	if(hnd->data_level == SNET_OBSERVERS_DATA_LEVEL_FULL 
	   && (interface = SNetInIdToInterface(interfaces, id)) != NULL){
	  fprintf(file,"<field label=\"%s\" interface=\"%s\" >", label, interface);

	  fun(file, SNetRecGetField(rec, i));

	  fprintf(file,"</field>");
	}else{
	  fprintf(file,"<field label=\"%s\" />", label);
	}
      }
      
      SNetMemFree(label);
    }

    /* tags */
    for(k=0; k<SNetRecGetNumTags( rec); k++) {
      i = SNetRecGetTagNames( rec)[k];
      
      if((label = SNetInIdToLabel(labels, i)) != NULL){
	if(hnd->data_level == SNET_OBSERVERS_DATA_LEVEL_NONE) {
	  fprintf(file,"<tag label=\"%s\" />", label);
	}else {
	  fprintf(file,"<tag label=\"%s\" >%d</tag>", label, SNetRecGetTag(rec, i));	   
	}
      }
      
      SNetMemFree(label);
    }

    /* btags */
    for(k=0; k<SNetRecGetNumBTags( rec); k++) {
      i = SNetRecGetBTagNames( rec)[k];
      
      if((label = SNetInIdToLabel(labels, i)) != NULL){
	if(hnd->data_level == SNET_OBSERVERS_DATA_LEVEL_NONE) {
	  fprintf(file,"<btag label=\"%s\" />", label); 
	}else {
	  fprintf(file,"<btag label=\"%s\" >%d</btag>", label, SNetRecGetBTag(rec, i));
	}
      }
      
      SNetMemFree(label);
    }
    fprintf(file,"</record>");
    break;
  case REC_sync:
    fprintf(file,"<record type=\"sync\" />");
  case REC_collect: 
    fprintf(file,"<record type=\"collect\" />");
  case REC_sort_begin: 
    fprintf(file,"<record type=\"sort_begin\" />");
  case REC_sort_end: 
    fprintf(file,"<record type=\"sort_end\" />");
  case REC_terminate:
    fprintf(file,"<record type=\"terminate\" />");
    break;
  default:
    break;
  }
  fprintf(file,"</observer>");
 
  return 0;
}

/** <!--********************************************************************-->
 *
 * @fn int ObserverSend(obs_handle_t *hnd, snet_record_t *rec)
 *
 *   @brief  Send record to the listener.

 *
 *   @param hnd   Handle of the observer.
 *   @param rec   Record to be sent.
 *   @return      0 in case of success, -1 in case of failure.
 *
 ******************************************************************************/

static int ObserverSend(obs_handle_t *hnd, snet_record_t *rec)
{
  int ret;
  obs_socket_t *socket;
  obs_file_t *file;

  pthread_mutex_lock(&connection_mutex);

  if(mustTerminate == true || hnd == NULL || hnd->desc == NULL){
    pthread_mutex_unlock(&connection_mutex);
    return -1;
  }

  if(hnd->obstype == OBSfile) {
    file = (obs_file_t *)hnd->desc;

    ret = ObserverPrintRecordToFile(file->file, hnd, rec);
    
    pthread_mutex_unlock(&connection_mutex);
    return ret;

  } else if(hnd->obstype == OBSsocket) {
    socket = (obs_socket_t *)hnd->desc;
 
    ret = ObserverPrintRecordToFile(socket->file, hnd, rec);
    fflush(socket->file);
    
    if(hnd->isInteractive == true && ret == 0){    
      ObserverWait(hnd);    
    }
    
    pthread_mutex_unlock(&connection_mutex);
    return ret; 
  }

  pthread_mutex_unlock(&connection_mutex);
  return -1;
}

/** <!--********************************************************************-->
 *
 * @fn void *ObserverBoxThread( void *hndl) {
 *
 *   @brief  The main function for observer thread.
 *
 *           Observer takes incoming records, sends them to the listener,
 *           and after that passes the record to the next entity.
 *
 *   @param hndl  Handle of the observer.
 *   @return      NULL.
 *
 ******************************************************************************/

static void *ObserverBoxThread( void *hndl) 
{
  obs_handle_t *hnd = (obs_handle_t*)hndl; 
  snet_record_t *rec = NULL;

  bool isTerminated = false;
 
  /* Do until terminate record is processed. */
  while(!isTerminated){ 
    rec = SNetBufGet(hnd->inbuf);
    if(rec != NULL) {
           
      /* Send message. */
      ObserverSend(hnd, rec);
      
      if(SNetRecGetDescriptor(rec) == REC_terminate){
	isTerminated = true;
      }
      
      /* Pass the record to next component. */
      SNetBufPut(hnd->outbuf, rec);
    }
  } 

  /* Unregister observer */
  ObserverRemove(hnd);

  SNetBufBlockUntilEmpty(hnd->outbuf);
  SNetBufDestroy(hnd->outbuf);

  SNetMemFree(hnd);

  return NULL;
}

/** <!--********************************************************************-->
 *
 * @fn snet_buffer_t *SNetObserverSocketBox(snet_buffer_t *inbuf, 
 *				     const char *addr, 
 *				     int port, 
 *				     bool isInteractive, 
 *				     const char *position, 
 *				     char type, 
 *                                   char data_level, 
 *                                   const char *code)
 *
 *   @brief  Initialize an observer that uses socket communication.
 *
 *           Observer handle is created and the observer is started.
 *           Incase the connection cannot be opened, this observer is ignored.
 *
 *   @param inbuf         Buffer for incoming records.
 *   @param addr          Address of the listener.
 *   @param port          Port number that the listener uses.
 *   @param isInteractive True if this observer is interactive, false otherwise.
 *   @param position      String describing the position of this observer (Net/box name).
 *   @param type          Type of the observer SNET_OBSERVERS_TYPE_BEFORE or SNET_OBSERVERS_TYPE_AFTER
 *   @param data_level    Level of data sent by the observer: SNET_OBSERVERS_DATA_LEVEL_NONE or SNET_OBSERVERS_DATA_LEVEL_TAGS or SNET_OBSERVERS_DATA_LEVEL_FULL
 *   @param code          User specified code sent by the observer.
 *
 *   @return              Buffer for outcoming records.
 *
 ******************************************************************************/
snet_buffer_t *SNetObserverSocketBox(snet_buffer_t *inbuf, 
				     const char *addr, 
				     int port, 
				     bool isInteractive, 
				     const char *position, 
				     char type, 
				     char data_level, 
				     const char *code)
{
  pthread_t box_thread;
  obs_handle_t *hnd;

  hnd = SNetMemAlloc(sizeof(obs_handle_t));

  if(hnd == NULL){
    SNetMemFree(hnd);
    return inbuf;
  }

  hnd->inbuf = inbuf;

  pthread_mutex_lock(&connection_mutex);
  hnd->id = id_pool++;
  pthread_mutex_unlock(&connection_mutex);

  hnd->obstype = OBSsocket;

  /* Register observer */
  if(ObserverRegisterSocket(hnd, addr, port) != 0) {
    SNetMemFree(hnd);
    return inbuf;
  }

  hnd->outbuf  = SNetBufCreate( BUFFER_SIZE);

  hnd->type = type;
  hnd->isInteractive = isInteractive;
  hnd->position = position;
  hnd->data_level = data_level;
  hnd->code = code;

  pthread_create(&box_thread, NULL, ObserverBoxThread, (void*)hnd);
  pthread_detach(box_thread);

  return(hnd->outbuf);
}

/** <!--********************************************************************-->
 *
 * @fn snet_buffer_t *SNetObserverFileBox(snet_buffer_t *inbuf, 
 *				   const char *filename, 
 *				   const char *position, 
 *				   char type, 
 *				   char data_level, 
 *				   const char *code)
 *
 *   @brief  Initialize an observer that uses file for output.
 *
 *           Observer handle is created and the observer is started.
 *           Incase the file cannot be opened, this observer is ignored.
 *
 *   @param inbuf         Buffer for incoming records.
 *   @param filename      Name of the file to be used.
 *   @param position      String describing the position of this observer (Net/box name).
 *   @param type          Type of the observer SNET_OBSERVERS_TYPE_BEFORE or SNET_OBSERVERS_TYPE_AFTER
 *   @param data_level    Level of data sent by the observer: SNET_OBSERVERS_DATA_LEVEL_NONE or SNET_OBSERVERS_DATA_LEVEL_TAGS or SNET_OBSERVERS_DATA_LEVEL_FULL
 *   @param code          User specified code sent by the observer.
 *
 *   @return              Buffer for outcoming records.
 *
 ******************************************************************************/
snet_buffer_t *SNetObserverFileBox(snet_buffer_t *inbuf, 
				   const char *filename, 
				   const char *position, 
				   char type, 
				   char data_level, 
				   const char *code)
{
  pthread_t box_thread;
  obs_handle_t *hnd;
  
  hnd = SNetMemAlloc(sizeof(obs_handle_t));
  if(hnd == NULL){
    SNetMemFree(hnd);
    return inbuf;
  }

  hnd->inbuf = inbuf;
  hnd->outbuf  = SNetBufCreate( BUFFER_SIZE);

  pthread_mutex_lock(&connection_mutex);
  hnd->id = id_pool++;
  pthread_mutex_unlock(&connection_mutex);

  hnd->obstype = OBSfile;

  if(ObserverRegisterFile(hnd, filename) != 0) {
    SNetMemFree(hnd);
    return inbuf;
  }

  hnd->type = type;
  hnd->isInteractive = false;
  hnd->position = position;
  hnd->data_level = data_level;
  hnd->code = code;

  pthread_create(&box_thread, NULL, ObserverBoxThread, (void*)hnd);
  pthread_detach(box_thread);

  return(hnd->outbuf);
}

/** <!--********************************************************************-->
 *
 * @fn int SNetObserverInit(snetin_label_t *labs, snetin_interface_t *interfs) 
 *
 *   @brief  Call to this function initializes the observer system.
 *
 *           Notice: This function must be called before any other observer
 *           function!
 *
 *   @param labs     Labels used in the S-Net
 *   @param interfs  Interfaces used in the S-Net
 *   @return         0 if success, -1 otherwise.
 *
 ******************************************************************************/
int SNetObserverInit(snetin_label_t *labs, snetin_interface_t *interfs) 
{
  /* Start dispatcher */ 
  mustTerminate = 0;
  labels = labs;
  interfaces = interfs; 

  preg = SNetMemAlloc(sizeof(regex_t));

  if(preg == NULL){
    return -1; 
  }

  /* Compile regex pattern for parsing reply messages. */
  if(regcomp(preg, pattern, REG_EXTENDED) != 0){
    printf("Did not compile!\n");
    return -1;
  }
  
  /* Open notification pipe. */
  if(pipe(notification_pipe) == -1){
    return -1;
  }

  if((dispatcher_thread = SNetMemAlloc(sizeof(pthread_t))) == NULL)
  {
    return -1;
  }

  pthread_create(dispatcher_thread, NULL, ObserverDispatch, NULL);

  return 0;
}


/** <!--********************************************************************-->
 *
 * @fn void SNetObserverDestroy() 
 *
 *   @brief  Call to this function destroys the observer system.
 *
 *           Notice: No observer functions must be called after this call!
 *
 *
 ******************************************************************************/
void SNetObserverDestroy() 
{
  obs_socket_t *socket;
  obs_file_t *file; 
  obs_wait_queue_t *queue;

  /* Don't close connections before the dispatcher stops using them! */
  
  pthread_mutex_lock(&connection_mutex);
  mustTerminate = true;
  pthread_mutex_unlock(&connection_mutex);

  write(notification_pipe[1], "?", 1);

  if(pthread_join(*dispatcher_thread, NULL) != 0)
  {
    /* Error */
  }

  SNetMemFree(dispatcher_thread);

  while(open_sockets != NULL){
    socket = open_sockets->next;
    
    fclose(open_sockets->file);

    while(open_sockets->wait_queue != NULL) {
      queue = open_sockets->wait_queue->next;
      
      pthread_cond_destroy(&open_sockets->wait_queue->cond);
      
      SNetMemFree(open_sockets->wait_queue);
      
      open_sockets->wait_queue = queue;
    }
    
    SNetMemFree(open_sockets);
    
    open_sockets = socket;
  }

  while(open_files != NULL){
    file = open_files->next;
    
    fclose(open_files->file);

    SNetMemFree(open_files->filename);
    SNetMemFree(open_files);
    
    open_files = file;
  }

  regfree(preg);

  SNetMemFree(preg);

  return;
}


#undef MSG_BUF_SIZE 
#undef NOTIFICATION_BUFFER_SIZE
#undef BUF_SIZE 





